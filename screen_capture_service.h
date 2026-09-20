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

#ifndef SCREEN_CAPTURE_SERVICE_H
#define SCREEN_CAPTURE_SERVICE_H

#include <binder/BinderService.h>
#include <binder/IInterface.h>
#include <binder/Parcel.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <utils/Mutex.h>

namespace android {

/*
 * IScreenCaptureService — a tiny dedicated Binder interface.  Lives
 * alongside MiniSurfaceFlinger in the HWC plugin process.  The GStreamer
 * process looks it up by name and fetches the BufferQueue consumer end
 * plus the capture dimensions via Binder.
 *
 * Registered as "sailfish.screencap" so it does not collide with any
 * AOSP service name.
 */

class IScreenCaptureService : public IInterface
{
public:
    DECLARE_META_INTERFACE(ScreenCaptureService);
    virtual sp<IGraphicBufferConsumer> getConsumer() = 0;
    virtual int getWidth() = 0;
    virtual int getHeight() = 0;
    virtual sp<IGraphicBufferProducer> getProducer(int *width, int *height,
                                                    int *fps,
                                                    int64_t *generation) = 0;
    virtual int64_t registerProducer(const sp<IGraphicBufferProducer>& producer,
                                     int width, int height, int fps) = 0;
    virtual void unregisterProducer(int64_t generation) = 0;
};

class BnScreenCaptureService : public BnInterface<IScreenCaptureService>
{
public:
    virtual status_t onTransact(uint32_t code, const Parcel& data,
                                Parcel* reply, uint32_t flags = 0);
};

class BpScreenCaptureService : public BpInterface<IScreenCaptureService>
{
public:
    BpScreenCaptureService(const sp<IBinder>& impl)
        : BpInterface<IScreenCaptureService>(impl) {}

    sp<IGraphicBufferConsumer> getConsumer() override;
    int getWidth() override;
    int getHeight() override;
    sp<IGraphicBufferProducer> getProducer(int *width, int *height, int *fps,
                                            int64_t *generation) override;
    int64_t registerProducer(const sp<IGraphicBufferProducer>& producer,
                             int width, int height, int fps) override;
    void unregisterProducer(int64_t generation) override;
};

/*
 * ScreenCaptureService — the actual Binder service instantiated in the
 * HWC plugin process.
 *
 * It stores the legacy raw-capture consumer and, independently, one
 * MediaCodec input-Surface producer shared across processes. A generation
 * invalidates stale QPA targets when the recorder stops or restarts.
 */
class ScreenCaptureService
    : public BinderService<ScreenCaptureService>,
      public BnScreenCaptureService
{
public:
    static char const *getServiceName() { return "sailfish.screencap"; }

    static void setConsumer(const sp<IGraphicBufferConsumer>& c,
                            int width, int height) {
        Mutex::Autolock l(sLock);
        sConsumer = c;
        sWidth = width;
        sHeight = height;
    }

    static int64_t setProducer(const sp<IGraphicBufferProducer>& producer,
                               int width, int height, int fps) {
        if (producer == NULL || width <= 0 || height <= 0 || fps <= 0) return 0;

        Mutex::Autolock l(sLock);
        sProducer = producer;
        sProducerWidth = width;
        sProducerHeight = height;
        sProducerFps = fps;
        ++sProducerGeneration;
        if (sProducerGeneration == 0) ++sProducerGeneration;
        return sProducerGeneration;
    }

    static void clearProducer(int64_t generation) {
        Mutex::Autolock l(sLock);
        if (generation != 0 && generation == sProducerGeneration) {
            sProducer.clear();
            sProducerWidth = 0;
            sProducerHeight = 0;
            sProducerFps = 0;
            ++sProducerGeneration;
            if (sProducerGeneration == 0) ++sProducerGeneration;
        }
    }

    static bool getProducerState(sp<IGraphicBufferProducer> *producer,
                                 int *width, int *height, int *fps,
                                 int64_t *generation) {
        Mutex::Autolock l(sLock);
        if (sProducer == NULL || sProducerGeneration == 0) return false;
        if (producer) *producer = sProducer;
        if (width) *width = sProducerWidth;
        if (height) *height = sProducerHeight;
        if (fps) *fps = sProducerFps;
        if (generation) *generation = sProducerGeneration;
        return true;
    }

    static bool isProducerGenerationCurrent(int64_t generation) {
        Mutex::Autolock l(sLock);
        return generation != 0 && generation == sProducerGeneration &&
               sProducer != NULL;
    }

    sp<IGraphicBufferConsumer> getConsumer() override {
        Mutex::Autolock l(sLock);
        return sConsumer;
    }

    int getWidth() override {
        Mutex::Autolock l(sLock);
        return sWidth;
    }

    int getHeight() override {
        Mutex::Autolock l(sLock);
        return sHeight;
    }

    sp<IGraphicBufferProducer> getProducer(int *width, int *height, int *fps,
                                            int64_t *generation) override {
        sp<IGraphicBufferProducer> producer;
        if (!getProducerState(&producer, width, height, fps, generation)) {
            return NULL;
        }
        return producer;
    }

    int64_t registerProducer(const sp<IGraphicBufferProducer>& producer,
                             int width, int height, int fps) override {
        return setProducer(producer, width, height, fps);
    }

    void unregisterProducer(int64_t generation) override {
        clearProducer(generation);
    }

private:
    static sp<IGraphicBufferConsumer> sConsumer;
    static int sWidth;
    static int sHeight;
    static sp<IGraphicBufferProducer> sProducer;
    static int sProducerWidth;
    static int sProducerHeight;
    static int sProducerFps;
    static int64_t sProducerGeneration;
    static Mutex sLock;
};

} // namespace android

#endif // SCREEN_CAPTURE_SERVICE_H
