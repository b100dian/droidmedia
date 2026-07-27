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

#ifndef SCREEN_CAPTURE_MEDIASOURCE_H
#define SCREEN_CAPTURE_MEDIASOURCE_H

#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MetaData.h>
#include <gui/IGraphicBufferConsumer.h>
#include <utils/RefBase.h>

namespace android {

/*
 * ScreenCaptureMediaSource
 *
 * Wraps an IGraphicBufferConsumer and implements the metadata-mode
 * MediaSource contract.  On read(), it acquires a GraphicBuffer from
 * the consumer and writes a VideoGrallocMetadata struct into the
 * encoder-provided MediaBuffer, telling the hardware encoder to read
 * pixel data directly from the GraphicBuffer handle.
 *
 * Buffer release is handled automatically via the CaptureInputBuffer
 * subclass destructor — when the encoder releases the MediaBuffer,
 * the GraphicBuffer is returned to the consumer.
 */
class ScreenCaptureMediaSource : public MediaSource {
public:
    ScreenCaptureMediaSource(const sp<IGraphicBufferConsumer>& consumer,
                             int width, int height, int colorFormat);
    virtual ~ScreenCaptureMediaSource();

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

#if ANDROID_MAJOR >= 9
    virtual status_t read(MediaBufferBase **buffer,
                          const ReadOptions *options = NULL);
#else
    virtual status_t read(MediaBuffer **buffer,
                          const ReadOptions *options = NULL);
#endif

private:
    sp<IGraphicBufferConsumer> mConsumer;
    int mWidth;
    int mHeight;
    int mColorFormat;
    bool mStarted;

    ScreenCaptureMediaSource(const ScreenCaptureMediaSource&);
    ScreenCaptureMediaSource& operator=(const ScreenCaptureMediaSource&);
};

} // namespace android

#endif // SCREEN_CAPTURE_MEDIASOURCE_H