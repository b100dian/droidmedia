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

#ifndef SCREEN_CAPTURE_ENCODER_H
#define SCREEN_CAPTURE_ENCODER_H

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _ScreenCaptureEncoder ScreenCaptureEncoder;

typedef struct {
    void *data;
    ssize_t size;
    int64_t timestamp_ns;
    bool sync;
    bool codec_config;
} ScreenCaptureEncodedFrame;

typedef struct {
    void (*data_available)(void *user, const ScreenCaptureEncodedFrame *frame);
    void (*error)(void *user, int err);
    void (*eos)(void *user);
} ScreenCaptureEncoderCallbacks;

/*
 * Create an encoder + ScreenCaptureMediaSource pipeline.
 *
 * queue      — BufferQueue consumer (from droid_media_screen_capture_consumer_new)
 * width      — frame width
 * height     — frame height
 * colorFormat — OMX_COLOR_FormatAndroidOpaque or similar
 * bitrate    — target bitrate in bps
 * fps        — frame rate
 * metadataMode — request metadata input; copy mode is used by default and
 *                metadata mode falls back to copy mode if codec creation fails
 * callbacks  — output callbacks
 * cbUser     — passed to callbacks
 */
ScreenCaptureEncoder *
screen_capture_encoder_new(void *queue, int width, int height,
                           int colorFormat, int bitrate, int fps,
                           bool metadataMode,
                           const ScreenCaptureEncoderCallbacks *callbacks,
                           void *cbUser);

void screen_capture_encoder_destroy(ScreenCaptureEncoder *enc);

bool screen_capture_encoder_start(ScreenCaptureEncoder *enc);
void screen_capture_encoder_stop(ScreenCaptureEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_CAPTURE_ENCODER_H