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

/*
 * One encoded H.264 access unit (or codec configuration blob).
 *
 * `data` is valid only for the duration of the data_available callback;
 * consumers must copy it before returning.
 *
 * `timestamp_us` is the MediaCodec presentation timestamp: absolute
 * CLOCK_MONOTONIC microseconds as submitted by the producer. Idle display
 * periods appear as gaps; no frames are duplicated.
 *
 * `flags` carries the raw platform flags for diagnostics only. Consumers
 * must use the semantic fields:
 *   - codec_config: SPS/PPS data, not a picture; timestamp is not meaningful.
 *   - sync: IDR/key frame that can start decoding.
 * End of stream is reported through the `eos` callback, never as payload.
 */
typedef struct {
    const uint8_t *data;
    size_t size;
    int64_t timestamp_us;
    uint32_t flags;
    bool sync;
    bool codec_config;
} ScreenCaptureSurfaceEncodedFrame;

typedef struct {
    void (*data_available)(void *user,
                           const ScreenCaptureSurfaceEncodedFrame *frame);
    void (*format_changed)(void *user, int width, int height);
    void (*error)(void *user, int error);
    /* Invoked exactly once, after every preceding access unit has been
     * delivered, when the codec reaches end of stream during finish(). */
    void (*eos)(void *user);
} ScreenCaptureSurfaceEncoderCallbacks;

/*
 * Configure a hardware AVC encoder with an input Surface. The input producer
 * is registered with sailfish.screencap when the encoder is started; QPA
 * obtains the corresponding Surface through the opaque libminisf bridge.
 *
 * Callbacks are invoked from the encoder's output drain thread and never
 * while an internal encoder lock is held. They must return promptly and must
 * not call stop(), finish(), or destroy() on the same encoder.
 *
 * An encoder object is single-use: create, start, finish or stop, destroy.
 */
ScreenCaptureSurfaceEncoder *screen_capture_surface_encoder_new(
    int width, int height, int bitrate, int fps,
    const ScreenCaptureSurfaceEncoderCallbacks *callbacks, void *user);

bool screen_capture_surface_encoder_start(ScreenCaptureSurfaceEncoder *encoder);

/*
 * Graceful end of recording. Unregisters the producer, signals end of input
 * to the codec, drains remaining output until codec EOS or `timeout_ms`
 * (a non-positive value selects a default bound), invokes the eos callback,
 * then stops the codec. Returns true only if codec EOS was reached and
 * delivered. On timeout or error the codec is force-stopped and false is
 * returned; the eos callback is not invoked in that case.
 */
bool screen_capture_surface_encoder_finish(ScreenCaptureSurfaceEncoder *encoder,
                                           int timeout_ms);

/*
 * Immediate, bounded, idempotent stop. Pending codec output is discarded.
 * If a finish() is in progress on another thread, stop() forces it to
 * conclude and returns after the encoder has stopped.
 */
void screen_capture_surface_encoder_stop(ScreenCaptureSurfaceEncoder *encoder);

/* Calls stop() if still running, then releases all resources. */
void screen_capture_surface_encoder_destroy(ScreenCaptureSurfaceEncoder *encoder);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_CAPTURE_SURFACE_ENCODER_H */
