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
    GET_PRODUCER,
    REGISTER_PRODUCER,
    UNREGISTER_PRODUCER,
};

sp<IGraphicBufferConsumer> ScreenCaptureService::sConsumer;
int ScreenCaptureService::sWidth = 0;
int ScreenCaptureService::sHeight = 0;
sp<IGraphicBufferProducer> ScreenCaptureService::sProducer;
int ScreenCaptureService::sProducerWidth = 0;
int ScreenCaptureService::sProducerHeight = 0;
int ScreenCaptureService::sProducerFps = 0;
int64_t ScreenCaptureService::sProducerGeneration = 0;
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
    case GET_PRODUCER: {
        CHECK_INTERFACE(IScreenCaptureService, data, reply);
        int width = 0;
        int height = 0;
        int fps = 0;
        int64_t generation = 0;
        sp<IGraphicBufferProducer> producer = getProducer(
            &width, &height, &fps, &generation);
        reply->writeNoException();
        reply->writeStrongBinder(IInterface::asBinder(producer));
        reply->writeInt32(width);
        reply->writeInt32(height);
        reply->writeInt32(fps);
        reply->writeInt64(generation);
        return NO_ERROR;
    }
    case REGISTER_PRODUCER: {
        CHECK_INTERFACE(IScreenCaptureService, data, reply);
        sp<IBinder> binder = data.readStrongBinder();
        sp<IGraphicBufferProducer> producer;
        if (binder != NULL) {
            producer = interface_cast<IGraphicBufferProducer>(binder);
        }
        const int width = data.readInt32();
        const int height = data.readInt32();
        const int fps = data.readInt32();
        const int64_t generation = registerProducer(producer, width, height, fps);
        reply->writeNoException();
        reply->writeInt64(generation);
        return NO_ERROR;
    }
    case UNREGISTER_PRODUCER: {
        CHECK_INTERFACE(IScreenCaptureService, data, reply);
        unregisterProducer(data.readInt64());
        reply->writeNoException();
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

sp<IGraphicBufferProducer> BpScreenCaptureService::getProducer(
    int *width, int *height, int *fps, int64_t *generation)
{
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    status_t err = remote()->transact(GET_PRODUCER, data, &reply);
    if (err != NO_ERROR) {
        ALOGE("ScreenCaptureService getProducer transact failed: %d", err);
        return NULL;
    }
    int32_t exception = reply.readExceptionCode();
    if (exception != 0) {
        ALOGE("ScreenCaptureService getProducer exception: %d", exception);
        return NULL;
    }
    sp<IBinder> binder = reply.readStrongBinder();
    int outWidth = reply.readInt32();
    int outHeight = reply.readInt32();
    int outFps = reply.readInt32();
    int64_t outGeneration = reply.readInt64();
    if (width) *width = outWidth;
    if (height) *height = outHeight;
    if (fps) *fps = outFps;
    if (generation) *generation = outGeneration;
    return binder != NULL ? interface_cast<IGraphicBufferProducer>(binder) : NULL;
}

int64_t BpScreenCaptureService::registerProducer(
    const sp<IGraphicBufferProducer>& producer, int width, int height, int fps)
{
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    data.writeStrongBinder(IInterface::asBinder(producer));
    data.writeInt32(width);
    data.writeInt32(height);
    data.writeInt32(fps);

    status_t err = remote()->transact(REGISTER_PRODUCER, data, &reply);
    if (err != NO_ERROR) {
        ALOGE("ScreenCaptureService registerProducer transact failed: %d", err);
        return 0;
    }
    int32_t exception = reply.readExceptionCode();
    if (exception != 0) {
        ALOGE("ScreenCaptureService registerProducer exception: %d", exception);
        return 0;
    }
    return reply.readInt64();
}

void BpScreenCaptureService::unregisterProducer(int64_t generation)
{
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    data.writeInt64(generation);

    status_t err = remote()->transact(UNREGISTER_PRODUCER, data, &reply);
    if (err != NO_ERROR) {
        ALOGE("ScreenCaptureService unregisterProducer transact failed: %d", err);
        return;
    }
    int32_t exception = reply.readExceptionCode();
    if (exception != 0) {
        ALOGE("ScreenCaptureService unregisterProducer exception: %d", exception);
    }
}

IMPLEMENT_META_INTERFACE(ScreenCaptureService, "sailfish.screencap");

} // namespace android
