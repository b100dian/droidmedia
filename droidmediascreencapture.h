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

#ifndef DROID_MEDIA_SCREEN_CAPTURE_H
#define DROID_MEDIA_SCREEN_CAPTURE_H

#include "droidmedia.h"
#include "droidmediacodec.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encodes a hardware H.264 with android Surface as input.
 *
 * start() registers the input Surface with the sailfish.screencap service
 * (hosted by minisfservice). The qt5-qpa-hwcomposer-plugin renders the display
 * into it whenever the screen changes. Output is H.264 encoded.
 *
 * The object is single-use: new, start, finish or stop, destroy.
 */
typedef struct _DroidMediaScreenCapture DroidMediaScreenCapture;

/*
 * Callbacks are called on the encoder's output thread.
 * Must return promptly and must not call finish(), stop() or destroy().
 */
typedef struct {
  /* One access unit, or the SPS/PPS when codec_config is set. ts is the
   * presentation time the compositor gave the frame, in CLOCK_MONOTONIC ns */
  void (* data_available)(void *data, DroidMediaCodecData *encoded);
  void (* error)(void *data, int err);
  /* Called once, after the last access unit, if finish() reaches end of stream */
  void (* signal_eos)(void *data);
} DroidMediaScreenCaptureCallbacks;

/* Returns NULL if no hardware AVC encoder accepts the configuration, or if
 * libdroidmedia was built without screen capture (Android < 8). */
DroidMediaScreenCapture *droid_media_screen_capture_new(int width, int height, int bitrate,
                                                        int fps,
                                                        DroidMediaScreenCaptureCallbacks *cb,
                                                        void *data);

/* Starts the encoder, that publishes its input Surface to the compositor. */
bool droid_media_screen_capture_start(DroidMediaScreenCapture *capture);

/*
 * Ends the recording: removes the Surface from the compositor,
 * signals end of input and delivers the remaining output, then stops.
 * Negative timeout means use default value (2000ms). Returns true if the
 * end of the stream was reached; otherwise the encoder is stopped anyway.
 */
bool droid_media_screen_capture_finish(DroidMediaScreenCapture *capture, int timeout_ms);

/*
 * Stops immediately and discards pending output. Idempotent; if finish() is
 * running on another thread, cuts it short and waits for it.
 */
void droid_media_screen_capture_stop(DroidMediaScreenCapture *capture);

/* Stops if needed, then frees the object. */
void droid_media_screen_capture_destroy(DroidMediaScreenCapture *capture);

#ifdef __cplusplus
};
#endif

#endif /* DROID_MEDIA_SCREEN_CAPTURE_H */
