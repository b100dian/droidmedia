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

#define DO_NOT_CHECK_MANUAL_BINDER_INTERFACES

#include "screen_capture_service.h"
#include <gui/IGraphicBufferConsumer.h>

namespace android {

enum {
    GET_CONSUMER = IBinder::FIRST_CALL_TRANSACTION,
    GET_WIDTH,
    GET_HEIGHT,
};

sp<IGraphicBufferConsumer> ScreenCaptureService::sConsumer;
int ScreenCaptureService::sWidth = 0;
int ScreenCaptureService::sHeight = 0;
Mutex ScreenCaptureService::sLock;

/* BnScreenCaptureService — same-process dispatch */
status_t BnScreenCaptureService::onTransact(uint32_t code, const Parcel& data,
                                            Parcel* reply, uint32_t flags)
{
    switch (code) {
    case GET_CONSUMER: {
        CHECK_INTERFACE(IScreenCaptureService, data, reply);
        sp<IGraphicBufferConsumer> consumer = getConsumer();
        reply->writeNoException();
        reply->writeStrongBinder(IInterface::asBinder(consumer));
        return NO_ERROR;
    }
    case GET_WIDTH: {
        CHECK_INTERFACE(IScreenCaptureService, data, reply);
        reply->writeNoException();
        reply->writeInt32(getWidth());
        return NO_ERROR;
    }
    case GET_HEIGHT: {
        CHECK_INTERFACE(IScreenCaptureService, data, reply);
        reply->writeNoException();
        reply->writeInt32(getHeight());
        return NO_ERROR;
    }
    default:
        return BBinder::onTransact(code, data, reply, flags);
    }
}

/* BpScreenCaptureService — cross-process proxy, called from GStreamer process */

sp<IGraphicBufferConsumer> BpScreenCaptureService::getConsumer()
{
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    status_t err = remote()->transact(GET_CONSUMER, data, &reply);
    if (err != NO_ERROR) {
        ALOGE("ScreenCaptureService getConsumer transact failed: %d", err);
        return NULL;
    }
    int32_t exception = reply.readExceptionCode();
    if (exception != 0) {
        ALOGE("ScreenCaptureService getConsumer exception: %d", exception);
        return NULL;
    }
    sp<IBinder> b = reply.readStrongBinder();
    if (b == NULL) {
        ALOGE("ScreenCaptureService getConsumer returned null binder");
        return NULL;
    }
    return interface_cast<IGraphicBufferConsumer>(b);
}

int BpScreenCaptureService::getWidth()
{
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    status_t err = remote()->transact(GET_WIDTH, data, &reply);
    if (err != NO_ERROR) {
        ALOGE("ScreenCaptureService getWidth transact failed: %d", err);
        return 0;
    }
    int32_t exception = reply.readExceptionCode();
    if (exception != 0) {
        ALOGE("ScreenCaptureService getWidth exception: %d", exception);
        return 0;
    }
    return reply.readInt32();
}

int BpScreenCaptureService::getHeight()
{
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    status_t err = remote()->transact(GET_HEIGHT, data, &reply);
    if (err != NO_ERROR) {
        ALOGE("ScreenCaptureService getHeight transact failed: %d", err);
        return 0;
    }
    int32_t exception = reply.readExceptionCode();
    if (exception != 0) {
        ALOGE("ScreenCaptureService getHeight exception: %d", exception);
        return 0;
    }
    return reply.readInt32();
}

IMPLEMENT_META_INTERFACE(ScreenCaptureService, "sailfish.screencap");

} // namespace android
