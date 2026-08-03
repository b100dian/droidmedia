/*
 * Copyright (C) 2026 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#ifndef SCREEN_CAPTURE_MEDIASOURCE_H
#define SCREEN_CAPTURE_MEDIASOURCE_H

#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MetaData.h>
#include <gui/BufferQueue.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/IConsumerListener.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>

namespace android {

/*
 * ScreenCaptureMediaSource owns the consumer-side BufferQueue listener and
 * implements the copy path accepted by the target Codec2 encoder. In copy
 * mode the GraphicBuffer is locked, copied into a regular MediaBuffer, and
 * released before read() returns. Metadata mode is retained for devices that
 * accept it; those buffers keep their acquired slot until their MediaBuffer
 * is released by the codec source.
 */
class ScreenCaptureMediaSource : public MediaSource,
                                 public MediaBufferObserver,
                                 protected ConsumerListener {
public:
    ScreenCaptureMediaSource(const sp<IGraphicBufferConsumer>& consumer,
                             int width, int height, int colorFormat,
                             bool metadataMode);
    virtual ~ScreenCaptureMediaSource();

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

#if ANDROID_MAJOR >= 9
    virtual status_t read(MediaBufferBase **buffer,
                          const ReadOptions *options = NULL);
    virtual void signalBufferReturned(MediaBufferBase *buffer);
#else
    virtual status_t read(MediaBuffer **buffer,
                          const ReadOptions *options = NULL);
    virtual void signalBufferReturned(MediaBuffer *buffer);
#endif

protected:
    virtual void onFrameAvailable(const BufferItem& item);
    virtual void onBuffersReleased();
    virtual void onSidebandStreamChanged();

private:
    sp<IGraphicBufferConsumer> mConsumer;
    sp<BufferQueue::ProxyConsumerListener> mListener;
    int mWidth;
    int mHeight;
    int mColorFormat;
    bool mMetadataMode;
    bool mStarted;
    bool mConnected;
    Mutex mLock;
    Condition mFrameAvailable;

    ScreenCaptureMediaSource(const ScreenCaptureMediaSource&);
    ScreenCaptureMediaSource& operator=(const ScreenCaptureMediaSource&);
};

} // namespace android

#endif // SCREEN_CAPTURE_MEDIASOURCE_H
