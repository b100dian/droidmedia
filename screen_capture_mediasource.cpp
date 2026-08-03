/*
 * Copyright (c) 2020 Open Mobile Platform LLC.
 * Copyright 2016, The Android Open Source Project
 * Copyright (C) 2026 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "screen_capture_mediasource.h"

#include <media/hardware/MetadataBufferType.h>
#include <media/stagefright/MediaDefs.h>
#include <utils/Log.h>
#include <ui/GraphicBuffer.h>
#include <ui/Fence.h>
#include <EGL/egl.h>
#include <inttypes.h>
#include <string.h>

#undef LOG_TAG
#define LOG_TAG "ScreenCaptureMS"
#define SCREEN_CAPTURE_METADATA_KEY 0x73636d64

namespace android {

class CaptureInputBuffer : public MediaBuffer {
public:
    CaptureInputBuffer(size_t size,
                       const sp<IGraphicBufferConsumer>& consumer,
                       int slot, uint64_t frameNumber)
        : MediaBuffer(size),
          mConsumer(consumer),
          mSlot(slot),
          mFrameNumber(frameNumber),
          mReleased(false) {
    }

    void releaseGraphicBuffer() {
        if (mReleased || mConsumer == NULL) {
            return;
        }

        status_t err = mConsumer->releaseBuffer(
            mSlot, mFrameNumber, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR,
            Fence::NO_FENCE);
        if (err != NO_ERROR && err != IGraphicBufferConsumer::STALE_BUFFER_SLOT) {
            ALOGW("releaseBuffer(slot=%d, frame=%" PRIu64 ") failed: %d",
                  mSlot, mFrameNumber, err);
        }
        mReleased = true;
    }

protected:
    virtual ~CaptureInputBuffer() {
        releaseGraphicBuffer();
    }

private:
    sp<IGraphicBufferConsumer> mConsumer;
    int mSlot;
    uint64_t mFrameNumber;
    bool mReleased;
};

ScreenCaptureMediaSource::ScreenCaptureMediaSource(
        const sp<IGraphicBufferConsumer>& consumer,
        int width, int height, int colorFormat, bool metadataMode)
    : mConsumer(consumer),
      mWidth(width),
      mHeight(height),
      mColorFormat(colorFormat),
      mMetadataMode(metadataMode),
      mStarted(false),
      mConnected(false) {
    ALOGI("created: %dx%d colorFormat=0x%x mode=%s", width, height,
          colorFormat, metadataMode ? "metadata" : "copy");
}

ScreenCaptureMediaSource::~ScreenCaptureMediaSource() {
    Mutex::Autolock lock(mLock);
    mStarted = false;
    mFrameAvailable.broadcast();
    if (mConnected) {
        mConsumer->consumerDisconnect();
        mConnected = false;
    }
    mListener.clear();
    ALOGI("destroyed");
}

status_t ScreenCaptureMediaSource::start(MetaData * /*params*/) {
    Mutex::Autolock lock(mLock);
    if (mStarted) {
        return INVALID_OPERATION;
    }

    if (!mConnected) {
        wp<ConsumerListener> listener = static_cast<ConsumerListener *>(this);
        mListener = new BufferQueue::ProxyConsumerListener(listener);
        status_t err = mConsumer->consumerConnect(mListener, false);
        if (err != NO_ERROR) {
            ALOGE("consumerConnect failed: %d", err);
            mListener.clear();
            return err;
        }
        mConnected = true;
    }

    mStarted = true;
    ALOGI("started");
    return OK;
}

status_t ScreenCaptureMediaSource::stop() {
    Mutex::Autolock lock(mLock);
    if (mStarted) {
        mStarted = false;
        mFrameAvailable.broadcast();
        ALOGI("stop requested");
        return NO_ERROR;
    }

    if (mConnected) {
        status_t err = mConsumer->consumerDisconnect();
        if (err != NO_ERROR && err != BAD_VALUE) {
            ALOGW("consumerDisconnect failed: %d", err);
        }
        mConnected = false;
        mListener.clear();
    }
    ALOGI("stopped");
    return NO_ERROR;
}

sp<MetaData> ScreenCaptureMediaSource::getFormat() {
    sp<MetaData> md = new MetaData;
    md->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
    md->setInt32(kKeyWidth, mWidth);
    md->setInt32(kKeyHeight, mHeight);
    md->setInt32(kKeyColorFormat, mColorFormat);
    md->setInt32(kKeyMaxInputSize, mWidth * mHeight * 4);
    md->setInt32(kKeyStride, mWidth);
    md->setInt32(kKeySliceHeight, mHeight);
    return md;
}

void ScreenCaptureMediaSource::onFrameAvailable(const BufferItem& /*item*/) {
    Mutex::Autolock lock(mLock);
    mFrameAvailable.broadcast();
}

void ScreenCaptureMediaSource::onBuffersReleased() {
    Mutex::Autolock lock(mLock);
    mFrameAvailable.broadcast();
}

void ScreenCaptureMediaSource::onSidebandStreamChanged() {
}

#if ANDROID_MAJOR >= 9
status_t ScreenCaptureMediaSource::read(
        MediaBufferBase **out, const ReadOptions * /*options*/) {
#else
status_t ScreenCaptureMediaSource::read(
        MediaBuffer **out, const ReadOptions * /*options*/) {
#endif
    if (!out) {
        return BAD_VALUE;
    }
    *out = NULL;

    for (;;) {
        {
            Mutex::Autolock lock(mLock);
            if (!mStarted) {
                return ERROR_END_OF_STREAM;
            }
        }

        BufferItem item;
        status_t err = mConsumer->acquireBuffer(&item, 0);
        if (err == NO_ERROR) {
            if (item.mGraphicBuffer == NULL) {
                ALOGW("acquireBuffer returned no GraphicBuffer for slot %d", item.mSlot);
                mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber,
                    EGL_NO_DISPLAY, EGL_NO_SYNC_KHR, Fence::NO_FENCE);
                continue;
            }

            if (mMetadataMode) {
                struct {
                    int32_t eType;
                    buffer_handle_t pHandle;
                } meta;

                meta.eType = kMetadataBufferTypeGrallocSource;
                meta.pHandle = item.mGraphicBuffer->handle;

                CaptureInputBuffer *buffer = new CaptureInputBuffer(
                    sizeof(meta), mConsumer, item.mSlot, item.mFrameNumber);
                memcpy(buffer->data(), &meta, sizeof(meta));
                buffer->set_range(0, sizeof(meta));
                buffer->setObserver(this);
                buffer->add_ref();
#if ANDROID_MAJOR >= 9
                buffer->meta_data().setInt32(SCREEN_CAPTURE_METADATA_KEY, 1);
                buffer->meta_data().setInt64(kKeyTime, item.mTimestamp);
#else
                buffer->meta_data()->setInt64(kKeyTime, item.mTimestamp);
#endif
                *out = buffer;
                return OK;
            }

            void *source = NULL;
            err = item.mGraphicBuffer->lock(
                GRALLOC_USAGE_SW_READ_OFTEN, &source);
            if (err != NO_ERROR || source == NULL) {
                ALOGE("GraphicBuffer::lock failed for slot %d: %d", item.mSlot, err);
                mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber,
                    EGL_NO_DISPLAY, EGL_NO_SYNC_KHR, Fence::NO_FENCE);
                return ERROR_END_OF_STREAM;
            }

            const size_t rowBytes = static_cast<size_t>(mWidth) * 4;
            const size_t frameSize = rowBytes * mHeight;
            MediaBuffer *buffer = new MediaBuffer(frameSize);
            const uint8_t *sourceBytes = static_cast<const uint8_t *>(source);
            uint8_t *destination = static_cast<uint8_t *>(buffer->data());
            const size_t sourceStride =
                static_cast<size_t>(item.mGraphicBuffer->getStride()) * 4;

            for (int y = 0; y < mHeight; ++y) {
                memcpy(destination + y * rowBytes,
                       sourceBytes + y * sourceStride, rowBytes);
            }
            item.mGraphicBuffer->unlock();
            mConsumer->releaseBuffer(item.mSlot, item.mFrameNumber,
                EGL_NO_DISPLAY, EGL_NO_SYNC_KHR, Fence::NO_FENCE);

            buffer->set_range(0, frameSize);
#if ANDROID_MAJOR >= 9
            buffer->meta_data().setInt64(kKeyTime, item.mTimestamp);
#else
            buffer->meta_data()->setInt64(kKeyTime, item.mTimestamp);
#endif
            *out = buffer;
            return OK;
        }

        if (err != BufferQueue::NO_BUFFER_AVAILABLE &&
            err != BufferQueue::PRESENT_LATER) {
            ALOGE("acquireBuffer returned %d (%s)", err, strerror(-err));
            return ERROR_END_OF_STREAM;
        }

        Mutex::Autolock lock(mLock);
        if (mStarted) {
            mFrameAvailable.waitRelative(mLock, 100000000LL);
        } else {
            return ERROR_END_OF_STREAM;
        }
    }
}

#if ANDROID_MAJOR >= 9
void ScreenCaptureMediaSource::signalBufferReturned(MediaBufferBase *buffer) {
#else
void ScreenCaptureMediaSource::signalBufferReturned(MediaBuffer *buffer) {
#endif
    CaptureInputBuffer *capture = static_cast<CaptureInputBuffer *>(buffer);
    capture->releaseGraphicBuffer();
    buffer->setObserver(NULL);
    buffer->release();
}

} // namespace android
