/*
 * Copyright (C) 2026 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef SCREEN_CAPTURE_SURFACE_ENCODER_H
#define SCREEN_CAPTURE_SURFACE_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ScreenCaptureSurfaceEncoder ScreenCaptureSurfaceEncoder;

typedef struct {
    const uint8_t *data;
    size_t size;
    int64_t timestamp_us;
    uint32_t flags;
} ScreenCaptureSurfaceEncodedFrame;

typedef struct {
    void (*data_available)(void *user,
                           const ScreenCaptureSurfaceEncodedFrame *frame);
    void (*format_changed)(void *user, int width, int height);
    void (*error)(void *user, int error);
    void (*eos)(void *user);
} ScreenCaptureSurfaceEncoderCallbacks;

/*
 * Configure a hardware AVC encoder with an input Surface. The input producer
 * is registered with sailfish.screencap when the encoder is started; QPA
 * obtains the corresponding Surface through the opaque libminisf bridge.
 */
ScreenCaptureSurfaceEncoder *screen_capture_surface_encoder_new(
    int width, int height, int bitrate, int fps,
    const ScreenCaptureSurfaceEncoderCallbacks *callbacks, void *user);

bool screen_capture_surface_encoder_start(ScreenCaptureSurfaceEncoder *encoder);
void screen_capture_surface_encoder_stop(ScreenCaptureSurfaceEncoder *encoder);
void screen_capture_surface_encoder_destroy(ScreenCaptureSurfaceEncoder *encoder);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_CAPTURE_SURFACE_ENCODER_H */
