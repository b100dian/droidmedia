/*
 * Copyright (C) 2026 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "screen_capture_service.h"

#include <binder/IServiceManager.h>
#include <gui/Surface.h>
#include <utils/Log.h>

#include <inttypes.h>

#undef LOG_TAG
#define LOG_TAG "MiniSfScreenCapture"

using namespace android;

typedef struct MinisfScreenCaptureSessionInfo {
    uint64_t generation;
    int width;
    int height;
    int fps;
} MinisfScreenCaptureSessionInfo;

namespace {

struct ScreenCaptureTarget {
    sp<IScreenCaptureService> service;
    sp<IGraphicBufferProducer> producer;
    sp<Surface> surface;
    int width;
    int height;
    int fps;
    int64_t generation;
};

static sp<IScreenCaptureService> screenCaptureService()
{
    sp<IServiceManager> manager = defaultServiceManager();
    if (manager == NULL) return NULL;

    sp<IBinder> binder = manager->getService(String16("sailfish.screencap"));
    if (binder == NULL) return NULL;
    return interface_cast<IScreenCaptureService>(binder);
}

static void *createScreenCaptureTarget(
    const sp<IScreenCaptureService>& service,
    const sp<IGraphicBufferProducer>& producer, int width, int height, int fps,
    int64_t generation)
{
    sp<IBinder> producerBinder = IInterface::asBinder(producer);
    ALOGI("received encoder producer Binder=%p local=%p remote=%p "
          "generation=%" PRId64,
          producerBinder.get(),
          producerBinder != NULL ? producerBinder->localBinder() : NULL,
          producerBinder != NULL ? producerBinder->remoteBinder() : NULL,
          generation);

    sp<Surface> surface = new Surface(producer, true);
    if (surface == NULL) return NULL;

    ScreenCaptureTarget *target = new ScreenCaptureTarget;
    target->service = service;
    target->producer = producer;
    target->surface = surface;
    target->width = width;
    target->height = height;
    target->fps = fps;
    target->generation = generation;

    ALOGI("acquired encoder Surface target %p generation=%" PRId64,
          target, generation);
    return target;
}

} // namespace

extern "C" {

int minisf_screen_capture_session_query(MinisfScreenCaptureSessionInfo *info)
{
    if (info == NULL) return 0;

    info->generation = 0;
    info->width = 0;
    info->height = 0;
    info->fps = 0;

    sp<IScreenCaptureService> service = screenCaptureService();
    if (service == NULL) return 0;

    int64_t generation = 0;
    sp<IGraphicBufferProducer> producer = service->getProducer(
        &info->width, &info->height, &info->fps, &generation);
    if (producer == NULL || generation == 0 || info->width <= 0 ||
        info->height <= 0 || info->fps <= 0) {
        info->width = 0;
        info->height = 0;
        info->fps = 0;
        return 0;
    }

    info->generation = static_cast<uint64_t>(generation);
    return 1;
}

void *minisf_screen_capture_target_acquire(int width, int height,
                                           uint64_t *generation)
{
    if (width <= 0 || height <= 0) return NULL;

    sp<IScreenCaptureService> service = screenCaptureService();
    if (service == NULL) return NULL;

    int producerWidth = 0;
    int producerHeight = 0;
    int producerFps = 0;
    int64_t producerGeneration = 0;
    sp<IGraphicBufferProducer> producer = service->getProducer(
        &producerWidth, &producerHeight, &producerFps, &producerGeneration);
    if (producer == NULL || producerGeneration == 0 ||
        producerWidth <= 0 || producerHeight <= 0 || producerFps <= 0) {
        return NULL;
    }

    /* A fixed-size EGL window must not silently scale a differently configured
     * encoder session. The recorder must restart with matching dimensions. */
    if (producerWidth != width || producerHeight != height) {
        ALOGW("encoder target is %dx%d, requested %dx%d",
              producerWidth, producerHeight, width, height);
        return NULL;
    }

    void *target = createScreenCaptureTarget(
        service, producer, producerWidth, producerHeight, producerFps,
        producerGeneration);
    if (target != NULL && generation) {
        *generation = static_cast<uint64_t>(producerGeneration);
    }
    return target;
}

void *minisf_screen_capture_target_acquire_generation(
    uint64_t expected_generation)
{
    if (expected_generation == 0) return NULL;

    sp<IScreenCaptureService> service = screenCaptureService();
    if (service == NULL) return NULL;

    int width = 0;
    int height = 0;
    int fps = 0;
    int64_t generation = 0;
    sp<IGraphicBufferProducer> producer = service->getProducer(
        &width, &height, &fps, &generation);
    if (producer == NULL || generation == 0 || width <= 0 || height <= 0 ||
        fps <= 0 || static_cast<uint64_t>(generation) != expected_generation) {
        return NULL;
    }

    return createScreenCaptureTarget(
        service, producer, width, height, fps, generation);
}

void *minisf_screen_capture_target_native_window(void *opaqueTarget)
{
    ScreenCaptureTarget *target =
        static_cast<ScreenCaptureTarget *>(opaqueTarget);
    /* Surface uses multiple inheritance through ANativeObjectBase. Preserve
     * the adjusted ANativeWindow base pointer across the opaque ABI; passing
     * Surface* directly is not guaranteed to point at ANativeWindow. */
    return target && target->surface
        ? static_cast<void *>(static_cast<ANativeWindow *>(target->surface.get()))
        : NULL;
}

void minisf_screen_capture_target_release(void *opaqueTarget)
{
    ScreenCaptureTarget *target =
        static_cast<ScreenCaptureTarget *>(opaqueTarget);
    if (target == NULL) return;

    ALOGI("releasing encoder Surface target %p generation=%" PRId64,
          target, target->generation);
    delete target;
}

int minisf_screen_capture_target_is_current(void *opaqueTarget,
                                            uint64_t generation)
{
    ScreenCaptureTarget *target =
        static_cast<ScreenCaptureTarget *>(opaqueTarget);
    if (target == NULL || generation == 0 ||
        generation != static_cast<uint64_t>(target->generation) ||
        target->service == NULL) {
        return 0;
    }

    int width = 0;
    int height = 0;
    int fps = 0;
    int64_t currentGeneration = 0;
    sp<IGraphicBufferProducer> producer = target->service->getProducer(
        &width, &height, &fps, &currentGeneration);
    return producer != NULL && currentGeneration == target->generation &&
           width == target->width && height == target->height &&
           fps == target->fps;
}

} // extern "C"
