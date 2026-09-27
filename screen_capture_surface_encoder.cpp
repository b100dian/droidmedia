/*
 * Copyright (C) 2026 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_capture_surface_encoder.h"
#include "screen_capture_service.h"

#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#include <media/MediaCodecBuffer.h>
#if ANDROID_MAJOR >= 11
#include <mediadrm/ICrypto.h>
#else
#include <media/ICrypto.h>
#endif
#include <media/openmax/OMX_Video.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <utils/Log.h>
#include <utils/Vector.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#undef LOG_TAG
#define LOG_TAG "ScreenCaptureSurfaceEnc"

using namespace android;

/* Default bound for finish() when the caller passes a non-positive timeout. */
static const int kDefaultFinishTimeoutMs = 2000;

/* QPA polls session state every 250 ms. After unregistering, keep the codec
 * and output drain alive for a bounded interval so an active display can
 * detach before the input consumer is stopped or signalled EOS. */
static const long kDetachGraceNs = 500000000L;

enum EncoderState {
    STATE_IDLE,      /* created, not started */
    STATE_STARTED,   /* codec running, producer published */
    STATE_FINISHING, /* finish() in progress: draining to codec EOS */
    STATE_STOPPING,  /* stop() tearing down */
    STATE_STOPPED    /* codec stopped; only destroy() remains */
};

struct _ScreenCaptureSurfaceEncoder {
    int mWidth;
    int mHeight;
    int mBitrate;
    int mFps;
    sp<ALooper> mLooper;
    sp<MediaCodec> mCodec;
    sp<IGraphicBufferProducer> mProducer;
    sp<IScreenCaptureService> mService;
    int64_t mGeneration;
    ScreenCaptureSurfaceEncoderCallbacks mCallbacks;
    void *mUser;
    pthread_t mThread;
    bool mThreadCreated;
    volatile bool mRunning;

    /* mLock protects the fields below. Callbacks are never invoked while
     * mLock is held. */
    pthread_mutex_t mLock;
    pthread_cond_t mCond;
    EncoderState mState;
    bool mOutputEos;    /* drain thread delivered codec EOS */
    bool mThreadExited; /* drain loop returned (EOS, error or stop) */
    bool mStopRequested; /* stop() called while finish() was waiting */
};

static void reportError(ScreenCaptureSurfaceEncoder *encoder, int error)
{
    if (encoder->mCallbacks.error) {
        encoder->mCallbacks.error(encoder->mUser, error);
    }
}

static void markThreadExited(ScreenCaptureSurfaceEncoder *encoder, bool eos)
{
    pthread_mutex_lock(&encoder->mLock);
    if (eos) encoder->mOutputEos = true;
    encoder->mThreadExited = true;
    pthread_cond_broadcast(&encoder->mCond);
    pthread_mutex_unlock(&encoder->mLock);
}

static void *surfaceEncoderThread(void *arg)
{
    ScreenCaptureSurfaceEncoder *encoder =
        static_cast<ScreenCaptureSurfaceEncoder *>(arg);
    bool sawEos = false;

    while (encoder->mRunning) {
        size_t index = 0;
        size_t offset = 0;
        size_t size = 0;
        int64_t timestampUs = 0;
        uint32_t flags = 0;
        status_t err = encoder->mCodec->dequeueOutputBuffer(
            &index, &offset, &size, &timestampUs, &flags, 100000LL);

        if (err == OK) {
            sp<MediaCodecBuffer> output;
            status_t bufferErr = encoder->mCodec->getOutputBuffer(index, &output);
            if (bufferErr != OK || output == NULL) {
                ALOGE("getOutputBuffer(%zu) failed: %d", index, bufferErr);
                encoder->mCodec->releaseOutputBuffer(index);
                reportError(encoder, bufferErr != OK ? bufferErr : UNKNOWN_ERROR);
                break;
            }

            if (size > 0 && encoder->mCallbacks.data_available) {
                ScreenCaptureSurfaceEncodedFrame frame;
                frame.data = output->base() + offset;
                frame.size = size;
                frame.timestamp_us = timestampUs;
                frame.flags = flags;
                frame.codec_config =
                    (flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) != 0;
                frame.sync = !frame.codec_config &&
                    (flags & MediaCodec::BUFFER_FLAG_SYNCFRAME) != 0;
                encoder->mCallbacks.data_available(encoder->mUser, &frame);
            }

            const bool eos = (flags & MediaCodec::BUFFER_FLAG_EOS) != 0;
            encoder->mCodec->releaseOutputBuffer(index);
            if (eos) {
                ALOGI("Surface encoder output EOS reached");
                /* Every preceding access unit has been delivered above. */
                if (encoder->mCallbacks.eos) {
                    encoder->mCallbacks.eos(encoder->mUser);
                }
                sawEos = true;
                break;
            }
        } else if (err == INFO_FORMAT_CHANGED) {
            sp<AMessage> format;
            if (encoder->mCodec->getOutputFormat(&format) == OK &&
                format != NULL && encoder->mCallbacks.format_changed) {
                int32_t width = 0;
                int32_t height = 0;
                format->findInt32("width", &width);
                format->findInt32("height", &height);
                encoder->mCallbacks.format_changed(encoder->mUser, width, height);
            }
        } else if (err == -EAGAIN || err == INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        } else if (err == -EPIPE || err == INVALID_OPERATION || !encoder->mRunning) {
            break;
        } else {
            ALOGE("dequeueOutputBuffer failed: %d", err);
            reportError(encoder, err);
            break;
        }
    }

    markThreadExited(encoder, sawEos);
    return NULL;
}

/* Invalidate the published generation so QPA detaches instead of swapping
 * against a consumer that is about to stop or receive EOS. Must be called
 * without mLock held (Binder call plus bounded sleep). */
static void unregisterProducer(ScreenCaptureSurfaceEncoder *encoder)
{
    if (encoder->mGeneration != 0 && encoder->mService != NULL) {
        ALOGI("unregistering Surface encoder generation=%" PRId64,
              encoder->mGeneration);
        encoder->mService->unregisterProducer(encoder->mGeneration);
        encoder->mGeneration = 0;

        /* If QPA is idle there is no producer swap to race; if a swap is
         * already blocked, the later codec stop still forces the BufferQueue
         * operation to return. */
        struct timespec grace = {0, kDetachGraceNs};
        while (nanosleep(&grace, &grace) != 0 && errno == EINTR) {}
    }
}

/* Stop the codec, join the drain thread and publish STATE_STOPPED. Exactly
 * one thread (the one that moved the state to FINISHING or STOPPING) may
 * call this. Must be called without mLock held. */
static void teardownCodec(ScreenCaptureSurfaceEncoder *encoder)
{
    /* stop() wakes dequeueOutputBuffer before joining the reader thread. */
    encoder->mRunning = false;
    status_t err = encoder->mCodec->stop();
    if (encoder->mThreadCreated) {
        void *result = NULL;
        pthread_join(encoder->mThread, &result);
        encoder->mThreadCreated = false;
    }
    encoder->mService.clear();
    if (err != OK && err != -EINVAL) ALOGW("Surface encoder stop: %d", err);

    pthread_mutex_lock(&encoder->mLock);
    encoder->mState = STATE_STOPPED;
    pthread_cond_broadcast(&encoder->mCond);
    pthread_mutex_unlock(&encoder->mLock);
}

static void absTimeAfterMs(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

extern "C" {

ScreenCaptureSurfaceEncoder *screen_capture_surface_encoder_new(
    int width, int height, int bitrate, int fps,
    const ScreenCaptureSurfaceEncoderCallbacks *callbacks, void *user)
{
    if (width <= 0 || height <= 0 || bitrate <= 0 || fps <= 0) return NULL;

    ScreenCaptureSurfaceEncoder *encoder = new ScreenCaptureSurfaceEncoder();
    encoder->mWidth = width;
    encoder->mHeight = height;
    encoder->mBitrate = bitrate;
    encoder->mFps = fps;
    encoder->mUser = user;
    encoder->mGeneration = 0;
    encoder->mThreadCreated = false;
    encoder->mRunning = false;
    encoder->mState = STATE_IDLE;
    encoder->mOutputEos = false;
    encoder->mThreadExited = false;
    encoder->mStopRequested = false;
    pthread_mutex_init(&encoder->mLock, NULL);
    /* finish() bounds its drain wait with this condvar; use the monotonic
     * clock so a wall-clock adjustment cannot shorten or extend it. */
    pthread_condattr_t condAttr;
    pthread_condattr_init(&condAttr);
    pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
    pthread_cond_init(&encoder->mCond, &condAttr);
    pthread_condattr_destroy(&condAttr);
    memset(&encoder->mCallbacks, 0, sizeof(encoder->mCallbacks));
    if (callbacks) encoder->mCallbacks = *callbacks;

    encoder->mLooper = new ALooper;
    encoder->mLooper->setName("ScreenCaptureSurfaceEncoder");
    encoder->mLooper->start();

    Vector<AString> components;
    MediaCodecList::findMatchingCodecs(
        MEDIA_MIMETYPE_VIDEO_AVC, true, MediaCodecList::kHardwareCodecsOnly,
        &components);
    for (size_t i = 0; i < components.size(); ++i) {
        encoder->mCodec = MediaCodec::CreateByComponentName(
            encoder->mLooper, components[i]);
        if (encoder->mCodec != NULL) {
            ALOGI("using hardware AVC component %s", components[i].c_str());
            break;
        }
    }
    if (encoder->mCodec == NULL) {
        ALOGE("no hardware AVC encoder component found");
        encoder->mLooper->stop();
        pthread_cond_destroy(&encoder->mCond);
        pthread_mutex_destroy(&encoder->mLock);
        delete encoder;
        return NULL;
    }

    sp<AMessage> format = new AMessage;
    format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
    format->setInt32("width", width);
    format->setInt32("height", height);
    format->setInt32("frame-rate", fps);
    format->setInt32("bitrate", bitrate);
    format->setInt32("i-frame-interval", 1);
    format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
    format->setInt32("prepend-sps-pps-to-idr-frames", 1);

    status_t err = encoder->mCodec->configure(
        format, NULL, NULL, MediaCodec::CONFIGURE_FLAG_ENCODE);
    if (err != OK) {
        ALOGE("Surface encoder configure failed: %d", err);
        encoder->mCodec->release();
        encoder->mCodec.clear();
        encoder->mLooper->stop();
        pthread_cond_destroy(&encoder->mCond);
        pthread_mutex_destroy(&encoder->mLock);
        delete encoder;
        return NULL;
    }

    err = encoder->mCodec->createInputSurface(&encoder->mProducer);
    if (err != OK || encoder->mProducer == NULL) {
        ALOGE("Surface encoder createInputSurface failed: %d", err);
        encoder->mCodec->release();
        encoder->mCodec.clear();
        encoder->mLooper->stop();
        pthread_cond_destroy(&encoder->mCond);
        pthread_mutex_destroy(&encoder->mLock);
        delete encoder;
        return NULL;
    }

    return encoder;
}

bool screen_capture_surface_encoder_start(ScreenCaptureSurfaceEncoder *encoder)
{
    if (encoder == NULL) return false;

    pthread_mutex_lock(&encoder->mLock);
    if (encoder->mState != STATE_IDLE) {
        pthread_mutex_unlock(&encoder->mLock);
        return false;
    }
    pthread_mutex_unlock(&encoder->mLock);

    /* Codec2 commonly returns an HIDL-to-Binder IGraphicBufferProducer adapter
     * hosted by this recorder process. Once it is published through
     * sailfish.screencap, the QPA producer connects to that local Binder
     * endpoint from another process. Start an inbound Binder pool before
     * registration; the same-process Gate-1 harness already does this. */
    ProcessState::self()->startThreadPool();

    sp<IServiceManager> serviceManager = defaultServiceManager();
    if (serviceManager == NULL) return false;
    sp<IBinder> binder = serviceManager->getService(
        String16("sailfish.screencap"));
    if (binder == NULL) {
        ALOGE("sailfish.screencap service is unavailable");
        return false;
    }
    encoder->mService = interface_cast<IScreenCaptureService>(binder);
    if (encoder->mService == NULL) return false;

    /* The producer must never be visible to QPA before MediaCodec is running:
     * EGL may connect and queue to it immediately after registration. */
    status_t err = encoder->mCodec->start();
    if (err != OK) {
        encoder->mService.clear();
        ALOGE("Surface encoder start failed: %d", err);
        return false;
    }

    encoder->mRunning = true;
    encoder->mOutputEos = false;
    encoder->mThreadExited = false;
    encoder->mStopRequested = false;
    int ret = pthread_create(&encoder->mThread, NULL, surfaceEncoderThread,
                             encoder);
    if (ret != 0) {
        encoder->mRunning = false;
        encoder->mCodec->stop();
        encoder->mService.clear();
        ALOGE("Surface encoder output thread creation failed: %d", ret);
        return false;
    }
    encoder->mThreadCreated = true;

    sp<IBinder> producerBinder = IInterface::asBinder(encoder->mProducer);
    ALOGI("publishing input producer Binder=%p local=%p remote=%p",
          producerBinder.get(),
          producerBinder != NULL ? producerBinder->localBinder() : NULL,
          producerBinder != NULL ? producerBinder->remoteBinder() : NULL);
    encoder->mGeneration = encoder->mService->registerProducer(
        encoder->mProducer, encoder->mWidth, encoder->mHeight, encoder->mFps);
    if (encoder->mGeneration == 0) {
        /* Prevent a running, unpublished input Surface from surviving a
         * failed start. The drain thread is stopped before the codec. */
        encoder->mRunning = false;
        encoder->mCodec->stop();
        pthread_join(encoder->mThread, NULL);
        encoder->mThreadCreated = false;
        encoder->mService.clear();
        ALOGE("Surface encoder producer registration failed");
        return false;
    }

    pthread_mutex_lock(&encoder->mLock);
    encoder->mState = STATE_STARTED;
    pthread_mutex_unlock(&encoder->mLock);
    ALOGI("Surface encoder ready: %dx%d @%d fps generation=%" PRId64,
          encoder->mWidth, encoder->mHeight, encoder->mFps,
          encoder->mGeneration);
    return true;
}

bool screen_capture_surface_encoder_finish(ScreenCaptureSurfaceEncoder *encoder,
                                           int timeout_ms)
{
    if (encoder == NULL) return false;
    if (timeout_ms <= 0) timeout_ms = kDefaultFinishTimeoutMs;

    pthread_mutex_lock(&encoder->mLock);
    if (encoder->mState != STATE_STARTED) {
        pthread_mutex_unlock(&encoder->mLock);
        return false;
    }
    encoder->mState = STATE_FINISHING;
    pthread_mutex_unlock(&encoder->mLock);

    unregisterProducer(encoder);

    bool result = false;
    /* The drain thread is still running: EOS propagates through it. */
    status_t err = encoder->mCodec->signalEndOfInputStream();
    if (err != OK) {
        ALOGE("signalEndOfInputStream failed: %d; forcing stop", err);
    } else {
        ALOGI("Surface encoder input EOS signalled; draining up to %d ms",
              timeout_ms);
        struct timespec deadline;
        absTimeAfterMs(&deadline, timeout_ms);

        pthread_mutex_lock(&encoder->mLock);
        int waitErr = 0;
        while (!encoder->mOutputEos && !encoder->mThreadExited &&
               !encoder->mStopRequested && waitErr != ETIMEDOUT) {
            waitErr = pthread_cond_timedwait(&encoder->mCond, &encoder->mLock,
                                             &deadline);
        }
        result = encoder->mOutputEos;
        if (!result) {
            ALOGW("Surface encoder finish did not reach codec EOS "
                  "(timeout=%d thread_exited=%d stop_requested=%d)",
                  waitErr == ETIMEDOUT, encoder->mThreadExited,
                  encoder->mStopRequested);
        }
        pthread_mutex_unlock(&encoder->mLock);
    }

    teardownCodec(encoder);
    return result;
}

void screen_capture_surface_encoder_stop(ScreenCaptureSurfaceEncoder *encoder)
{
    if (encoder == NULL) return;

    pthread_mutex_lock(&encoder->mLock);
    switch (encoder->mState) {
    case STATE_IDLE:
    case STATE_STOPPED:
        pthread_mutex_unlock(&encoder->mLock);
        return;
    case STATE_FINISHING:
    case STATE_STOPPING:
        /* Another thread owns teardown. Cut its drain wait short and wait for
         * it to publish STATE_STOPPED so this call is bounded by that path. */
        encoder->mStopRequested = true;
        pthread_cond_broadcast(&encoder->mCond);
        while (encoder->mState != STATE_STOPPED) {
            pthread_cond_wait(&encoder->mCond, &encoder->mLock);
        }
        pthread_mutex_unlock(&encoder->mLock);
        return;
    case STATE_STARTED:
        encoder->mState = STATE_STOPPING;
        break;
    }
    pthread_mutex_unlock(&encoder->mLock);

    unregisterProducer(encoder);
    teardownCodec(encoder);
}

void screen_capture_surface_encoder_destroy(ScreenCaptureSurfaceEncoder *encoder)
{
    if (encoder == NULL) return;
    screen_capture_surface_encoder_stop(encoder);
    encoder->mProducer.clear();
    if (encoder->mCodec != NULL) {
        encoder->mCodec->release();
        encoder->mCodec.clear();
    }
    if (encoder->mLooper != NULL) {
        encoder->mLooper->stop();
        encoder->mLooper.clear();
    }
    pthread_cond_destroy(&encoder->mCond);
    pthread_mutex_destroy(&encoder->mLock);
    delete encoder;
}

} // extern "C"
