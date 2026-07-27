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
 * screen_capture_mediasource.cpp
 *
 * Metadata-mode MediaSource that feeds GraphicBuffer handles to the
 * hardware encoder via OMX_COLOR_FormatAndroidOpaque.
 */

#include "screen_capture_mediasource.h"
#include "droidmediabuffer.h"
#include <media/stagefright/MediaDefs.h>
#include <media/hardware/MetadataBufferType.h>
#include <ui/GraphicBuffer.h>
#include <ui/Fence.h>
#include <EGL/egl.h>
#include <string.h>

#undef LOG_TAG
#define LOG_TAG "ScreenCaptureMS"

namespace android {

/* ----------------------------------------------------------------
 * Subclass of MediaBuffer that releases the GraphicBuffer back to
 * the consumer when the encoder is done with it.
 * ---------------------------------------------------------------- */
class CaptureInputBuffer : public MediaBuffer {
public:
    CaptureInputBuffer(size_t metaSize,
                       const sp<IGraphicBufferConsumer>& consumer,
                       int slot, int64_t frameNumber)
        : MediaBuffer(metaSize),
          mConsumer(consumer),
          mSlot(slot),
          mFrameNumber(frameNumber) {
    }

    virtual ~CaptureInputBuffer() {
        if (mConsumer != NULL) {
            mConsumer->releaseBuffer(mSlot, mFrameNumber,
                                     EGL_NO_DISPLAY, EGL_NO_SYNC_KHR,
                                     Fence::NO_FENCE);
        }
    }

private:
    sp<IGraphicBufferConsumer> mConsumer;
    int mSlot;
    int64_t mFrameNumber;
};

/* ----------------------------------------------------------------
 * ScreenCaptureMediaSource
 * ---------------------------------------------------------------- */

ScreenCaptureMediaSource::ScreenCaptureMediaSource(
        const sp<IGraphicBufferConsumer>& consumer,
        int width, int height, int colorFormat)
    : mConsumer(consumer),
      mWidth(width),
      mHeight(height),
      mColorFormat(colorFormat),
      mStarted(false) {
    ALOGI("created: %dx%d colorFormat=0x%x", width, height, colorFormat);
}

ScreenCaptureMediaSource::~ScreenCaptureMediaSource() {
    ALOGI("destroyed");
}

status_t ScreenCaptureMediaSource::start(MetaData * /*params*/) {
    mStarted = true;
    ALOGI("started");
    return OK;
}

status_t ScreenCaptureMediaSource::stop() {
    mStarted = false;
    ALOGI("stopped");
    return OK;
}

sp<MetaData> ScreenCaptureMediaSource::getFormat() {
    sp<MetaData> md = new MetaData;
    md->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
    md->setInt32(kKeyWidth, mWidth);
    md->setInt32(kKeyHeight, mHeight);
    md->setInt32(kKeyColorFormat, mColorFormat);
    return md;
}

#if ANDROID_MAJOR >= 9
status_t ScreenCaptureMediaSource::read(
        MediaBufferBase **out, const ReadOptions * /*options*/) {
#else
status_t ScreenCaptureMediaSource::read(
        MediaBuffer **out, const ReadOptions * /*options*/) {
#endif
    BufferItem item;
    status_t err = mConsumer->acquireBuffer(&item, 5000000000LL);
    if (err != OK) {
        ALOGE("acquireBuffer returned %d (%s)", err, strerror(-err));
        return ERROR_END_OF_STREAM;
    }

    /*
     * Metadata struct layout (VideoGrallocMetadata):
     *   int32_t eType  — kMetadataBufferTypeGrallocSource
     *   void*   pHandle — GraphicBuffer::handle (buffer_handle_t)
     */
    struct {
        int32_t eType;
        buffer_handle_t pHandle;
    } meta;

    meta.eType   = kMetadataBufferTypeGrallocSource;
    meta.pHandle = item.mGraphicBuffer->handle;

    CaptureInputBuffer *mb = new CaptureInputBuffer(
        sizeof(meta), mConsumer, item.mSlot, item.mFrameNumber);
    memcpy(mb->data(), &meta, sizeof(meta));
    mb->set_range(0, sizeof(meta));

#if ANDROID_MAJOR >= 9
    mb->meta_data().setInt64(kKeyTime, item.mTimestamp);
#else
    mb->meta_data()->setInt64(kKeyTime, item.mTimestamp);
#endif

    *out = mb;
    return OK;
}

} // namespace android
