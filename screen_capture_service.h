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
};

/*
 * ScreenCaptureService — the actual Binder service instantiated in the
 * HWC plugin process.
 *
 * It stores a single IGraphicBufferConsumer shared across processes,
 * along with the capture dimensions (since IGraphicBufferConsumer
 * doesn't expose getDefaultWidth/getDefaultHeight across Binder).
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

private:
    static sp<IGraphicBufferConsumer> sConsumer;
    static int sWidth;
    static int sHeight;
    static Mutex sLock;
};

} // namespace android

#endif // SCREEN_CAPTURE_SERVICE_H
