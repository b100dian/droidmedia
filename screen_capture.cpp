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
 * screen_capture.cpp — public C API for screen-capture BufferQueue setup.
 *
 * Called from the HWC plugin process to create the capture pipe,
 * and from the GStreamer process to fetch the consumer end + dimensions.
 */

#include <gui/BufferQueue.h>
#include <binder/IServiceManager.h>

#include "droidmedia.h"
#include "private.h"
#include "screen_capture_service.h"

#undef LOG_TAG
#define LOG_TAG "DroidScreenCapture"

using namespace android;

extern "C" {

void droid_media_screen_capture_init(int width, int height,
                                     DroidMediaBufferQueue **out_queue)
{
    if (!out_queue) return;

    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);

    consumer->setConsumerName(String8("ScreenCapture"));
    consumer->setDefaultBufferSize(width, height);
    consumer->setDefaultBufferFormat(HAL_PIXEL_FORMAT_RGBA_8888);
    consumer->setConsumerUsageBits(
        GraphicBuffer::USAGE_HW_TEXTURE |
        GraphicBuffer::USAGE_HW_VIDEO_ENCODER);
    status_t err = consumer->setMaxAcquiredBufferCount(3);
    if (err != NO_ERROR) {
        ALOGE("failed to set max acquired buffer count: %d", err);
        return;
    }

    DroidMediaBufferQueue *queue = new DroidMediaBufferQueue(producer);
    if (queue == NULL || queue->producer() == NULL) {
        ALOGE("failed to wrap screen capture producer");
        delete queue;
        return;
    }

    ScreenCaptureService::setConsumer(consumer, width, height);
    *out_queue = queue;

    ALOGI("screen capture init: %dx%d RGBA, consumer stored", width, height);
}

DroidMediaBufferQueue *droid_media_screen_capture_consumer_new(void)
{
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == NULL) {
        ALOGE("cannot get ServiceManager");
        return NULL;
    }

    sp<IBinder> b = sm->getService(String16("sailfish.screencap"));
    if (b == NULL) {
        ALOGW("sailfish.screencap service not found");
        return NULL;
    }

    sp<IScreenCaptureService> svc = interface_cast<IScreenCaptureService>(b);
    if (svc == NULL) {
        ALOGE("sailfish.screencap interface_cast failed");
        return NULL;
    }

    sp<IGraphicBufferConsumer> consumer = svc->getConsumer();
    if (consumer == NULL) {
        ALOGE("ScreenCaptureService has no consumer yet");
        return NULL;
    }

    DroidMediaBufferQueue *queue = new DroidMediaBufferQueue(consumer);
    if (queue == NULL) {
        ALOGE("Failed to allocate screen capture consumer wrapper");
        return NULL;
    }

    ALOGI("screen capture consumer wrapper created");
    return queue;
}

/*
 * droid_media_screen_capture_get_dimensions()
 *
 * Called from the GStreamer process.  Queries the ScreenCaptureService
 * for the capture dimensions via Binder.  Returns 0 on success.
 */
int droid_media_screen_capture_get_dimensions(int *width, int *height)
{
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == NULL) return -1;

    sp<IBinder> b = sm->getService(String16("sailfish.screencap"));
    if (b == NULL) return -1;

    sp<IScreenCaptureService> svc = interface_cast<IScreenCaptureService>(b);
    if (svc == NULL) return -1;

    int w = svc->getWidth();
    int h = svc->getHeight();
    if (w <= 0 || h <= 0) return -1;

    if (width)  *width  = w;
    if (height) *height = h;
    return 0;
}



void *droid_media_screen_capture_queue_producer(DroidMediaBufferQueue *queue)
{
    return queue ? queue->producer().get() : NULL;
}

void droid_media_screen_capture_queue_destroy(DroidMediaBufferQueue *queue)
{
    delete queue;
}

}; // extern "C"