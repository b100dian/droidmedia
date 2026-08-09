/*
 * Copyright (C) 2026 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_capture_surface_encoder.h"
#include "screen_capture_service.h"

#include <binder/IServiceManager.h>
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

#undef LOG_TAG
#define LOG_TAG "ScreenCaptureSurfaceEnc"

using namespace android;

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
    bool mStarted;
};

static void reportError(ScreenCaptureSurfaceEncoder *encoder, int error)
{
    if (encoder->mCallbacks.error) {
        encoder->mCallbacks.error(encoder->mUser, error);
    }
}

static void *surfaceEncoderThread(void *arg)
{
    ScreenCaptureSurfaceEncoder *encoder =
        static_cast<ScreenCaptureSurfaceEncoder *>(arg);

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
                encoder->mCallbacks.data_available(encoder->mUser, &frame);
            }

            const bool eos = (flags & MediaCodec::BUFFER_FLAG_EOS) != 0;
            encoder->mCodec->releaseOutputBuffer(index);
            if (eos) {
                if (encoder->mCallbacks.eos) {
                    encoder->mCallbacks.eos(encoder->mUser);
                }
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

    return NULL;
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
    encoder->mStarted = false;
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
    if (err != OK || encoder->mCodec->createInputSurface(&encoder->mProducer) != OK ||
        encoder->mProducer == NULL) {
        ALOGE("Surface encoder configure/createInputSurface failed: %d", err);
        encoder->mCodec->release();
        encoder->mCodec.clear();
        encoder->mLooper->stop();
        delete encoder;
        return NULL;
    }

    return encoder;
}

bool screen_capture_surface_encoder_start(ScreenCaptureSurfaceEncoder *encoder)
{
    if (encoder == NULL || encoder->mStarted) return false;

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

    encoder->mGeneration = encoder->mService->registerProducer(
        encoder->mProducer, encoder->mWidth, encoder->mHeight);
    if (encoder->mGeneration == 0) {
        encoder->mService.clear();
        return false;
    }

    status_t err = encoder->mCodec->start();
    if (err != OK) {
        encoder->mService->unregisterProducer(encoder->mGeneration);
        encoder->mGeneration = 0;
        encoder->mService.clear();
        ALOGE("Surface encoder start failed: %d", err);
        return false;
    }

    encoder->mRunning = true;
    encoder->mStarted = true;
    int ret = pthread_create(&encoder->mThread, NULL, surfaceEncoderThread,
                             encoder);
    if (ret != 0) {
        encoder->mRunning = false;
        encoder->mStarted = false;
        encoder->mCodec->stop();
        encoder->mService->unregisterProducer(encoder->mGeneration);
        encoder->mGeneration = 0;
        encoder->mService.clear();
        ALOGE("Surface encoder output thread creation failed: %d", ret);
        return false;
    }
    encoder->mThreadCreated = true;
    ALOGI("Surface encoder started: %dx%d @%d fps generation=%" PRId64,
          encoder->mWidth, encoder->mHeight, encoder->mFps,
          encoder->mGeneration);
    return true;
}

void screen_capture_surface_encoder_stop(ScreenCaptureSurfaceEncoder *encoder)
{
    if (encoder == NULL || !encoder->mStarted) return;

    /* stop() wakes dequeueOutputBuffer before joining the reader thread. */
    encoder->mRunning = false;
    status_t err = encoder->mCodec->stop();
    if (encoder->mThreadCreated) {
        void *result = NULL;
        pthread_join(encoder->mThread, &result);
        encoder->mThreadCreated = false;
    }
    if (encoder->mGeneration != 0) {
        encoder->mService->unregisterProducer(encoder->mGeneration);
        encoder->mGeneration = 0;
    }
    encoder->mService.clear();
    encoder->mStarted = false;
    if (err != OK && err != -EINVAL) ALOGW("Surface encoder stop: %d", err);
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
    delete encoder;
}

} // extern "C"
