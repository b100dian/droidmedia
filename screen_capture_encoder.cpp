/*
 * Copyright (C) 2026 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * screen_capture_encoder.cpp
 *
 * C-wrapper around the raw-encoder + ScreenCaptureMediaSource pipeline.
 * Manages a poll thread that reads encoded H.264 output and delivers it
 * via C callbacks — designed to be called from a GStreamer source element.
 */

#include "screen_capture_encoder.h"
#include "screen_capture_mediasource.h"
#include "private.h"
#include "droidmediacodec.h"
#include "droidmedia.h"

#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaSource.h>
#include <OMX_Video.h>
#include <pthread.h>
#include <string.h>

#undef LOG_TAG
#define LOG_TAG "ScreenCaptureEnc"

using namespace android;

struct _ScreenCaptureEncoder {
    sp<ScreenCaptureMediaSource> mSrc;
    sp<MediaSource> mCodec;
    sp<ALooper> mLooper;

    ScreenCaptureEncoderCallbacks mCb;
    void *mCbUser;

    bool mRunning;
    pthread_t mThread;
};

/* ----------------------------------------------------------------
 * Poll thread — mirrors DroidMediaRecorder::tick()
 * ---------------------------------------------------------------- */
static void *
encoder_thread(void *arg)
{
    ScreenCaptureEncoder *enc = (ScreenCaptureEncoder *)arg;
    status_t err = OK;

    while (enc->mRunning && err == OK) {
        MediaBuffer *buffer = NULL;

#if ANDROID_MAJOR >= 9
        err = enc->mCodec->read((MediaBufferBase **)&buffer);
#else
        err = enc->mCodec->read(&buffer);
#endif

        if (err == OK && buffer != NULL) {
            ScreenCaptureEncodedFrame frame;
            memset(&frame, 0, sizeof(frame));

            frame.data  = (uint8_t *)buffer->data() + buffer->range_offset();
            frame.size  = buffer->range_length();
            frame.timestamp_ns = 0;

            int32_t cfg = 0;
#if ANDROID_MAJOR >= 9
            if (buffer->meta_data().findInt32(kKeyIsCodecConfig, &cfg))
#else
            if (buffer->meta_data()->findInt32(kKeyIsCodecConfig, &cfg))
#endif
                frame.codec_config = (cfg != 0);

            int64_t ts = 0;
#if ANDROID_MAJOR >= 9
            if (buffer->meta_data().findInt64(kKeyTime, &ts))
#else
            if (buffer->meta_data()->findInt64(kKeyTime, &ts))
#endif
                frame.timestamp_ns = ts * 1000;

            int32_t sync = 0;
#if ANDROID_MAJOR >= 9
            buffer->meta_data().findInt32(kKeyIsSyncFrame, &sync);
#else
            buffer->meta_data()->findInt32(kKeyIsSyncFrame, &sync);
#endif
            frame.sync = (sync != 0);

            if (enc->mCb.data_available) {
                enc->mCb.data_available(enc->mCbUser, &frame);
            }

            buffer->release();
        } else if (err == INFO_FORMAT_CHANGED) {
            ALOGI("format changed, continuing...");
        } else if (err != -EWOULDBLOCK && err != OK) {
            if (err == ERROR_END_OF_STREAM || err == -ENODATA) {
                ALOGI("encoder signaled EOS");
                if (enc->mCb.eos) enc->mCb.eos(enc->mCbUser);
                break;
            } else {
                ALOGE("encoder read error: %d (0x%x)", err, -err);
                if (enc->mCb.error) enc->mCb.error(enc->mCbUser, err);
                break;
            }
        }
    }

    return NULL;
}

/* ----------------------------------------------------------------
 * C API
 * ---------------------------------------------------------------- */

extern "C" {

ScreenCaptureEncoder *
screen_capture_encoder_new(void *queuePtr, int width, int height,
                           int colorFormat, int bitrate, int fps,
                           const ScreenCaptureEncoderCallbacks *callbacks,
                           void *cbUser)
{
    DroidMediaBufferQueue *dmq = (DroidMediaBufferQueue *)queuePtr;
    if (!dmq) return NULL;

    sp<IGraphicBufferConsumer> consumer = dmq->consumer();
    if (consumer == NULL) {
        ALOGE("queue has no consumer");
        return NULL;
    }

    // Query dimensions from the ScreenCaptureService via Binder if not given
    if (width == 0 || height == 0) {
        int svcW = 0, svcH = 0;
        if (droid_media_screen_capture_get_dimensions(&svcW, &svcH) == 0
                && svcW > 0 && svcH > 0) {
            width = svcW;
            height = svcH;
            ALOGI("auto-detected dimensions from ScreenCaptureService: %dx%d",
                  width, height);
        } else {
            ALOGE("cannot determine dimensions (width=%d height=%d) — "
                  "pass width/height explicitly", width, height);
            return NULL;
        }
    }

    // Build encoder meta
    DroidMediaCodecEncoderMetaData md;
    memset(&md, 0, sizeof(md));
    md.parent.type   = "video/avc";
    md.parent.width  = width;
    md.parent.height = height;
    md.parent.fps    = fps;
    md.parent.flags  = DROID_MEDIA_CODEC_HW_ONLY;
    md.color_format  = colorFormat;
    md.bitrate       = bitrate;
    md.stride        = width;
    md.slice_height  = height;
    md.meta_data     = 1; // metadata-input mode

    // Build looper (must happen before any early-exit cleanup)
    sp<ALooper> looper = new ALooper;
    looper->setName("ScreenCapEncLooper");
    looper->start();

    // Build media source
    sp<ScreenCaptureMediaSource> src = new ScreenCaptureMediaSource(
        consumer, width, height, colorFormat);

    // Create raw encoder — returns sp<MediaSource>
    sp<MediaSource> codec = droid_media_codec_create_encoder_raw(
        &md, looper, src);
    if (codec == NULL) {
        ALOGE("droid_media_codec_create_encoder_raw failed");
        looper->stop();
        return NULL;
    }

    ScreenCaptureEncoder *enc = new ScreenCaptureEncoder;
    enc->mSrc     = src;
    enc->mCodec   = codec;
    enc->mLooper  = looper;
    enc->mRunning = false;
    memset(&enc->mCb, 0, sizeof(enc->mCb));
    enc->mCbUser = NULL;
    memset(&enc->mThread, 0, sizeof(enc->mThread));

    if (callbacks) {
        enc->mCb = *callbacks;
    }
    enc->mCbUser = cbUser;

    ALOGI("encoder pipeline created: %dx%d @%d fps, %d bps, cf=0x%x",
          width, height, fps, bitrate, colorFormat);

    return enc;
}

void screen_capture_encoder_destroy(ScreenCaptureEncoder *enc)
{
    if (!enc) return;

    screen_capture_encoder_stop(enc);

    enc->mCodec.clear();
    enc->mSrc.clear();
    enc->mLooper->stop();

    delete enc;
}

bool screen_capture_encoder_start(ScreenCaptureEncoder *enc)
{
    if (!enc || enc->mCodec == NULL) return false;

    status_t err = enc->mCodec->start();
    if (err != OK) {
        ALOGE("codec start failed: %d", err);
        return false;
    }

    enc->mRunning = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    int ret = pthread_create(&enc->mThread, &attr, encoder_thread, enc);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        ALOGE("pthread_create failed: %d", ret);
        enc->mRunning = false;
        return false;
    }

    ALOGI("encoder started, poll thread running");
    return true;
}

void screen_capture_encoder_stop(ScreenCaptureEncoder *enc)
{
    if (!enc || !enc->mRunning) return;

    enc->mRunning = false;
    void *dummy;
    pthread_join(enc->mThread, &dummy);

    status_t err = enc->mCodec->stop();
    if (err != OK) {
        ALOGE("codec stop error: %d", err);
    }

    ALOGI("encoder stopped");
}

} // extern "C"