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
 * Screen recording through a hardware H.264 encoder with Surface input.
 *
 * start() registers the encoder's input Surface with the sailfish.screencap
 * service (hosted by minisfservice). The compositor renders the display into
 * it whenever the screen changes, so idle periods are gaps in the timestamps,
 * not repeated frames. Output is H.264 byte-stream (Annex B).
 *
 * The object is single-use: new, start, finish or stop, destroy.
 */
typedef struct _DroidMediaScreenCapture DroidMediaScreenCapture;

/*
 * Called on the encoder's output thread, never with an internal lock held.
 * Callbacks must return promptly and must not call finish(), stop() or
 * destroy().
 */
typedef struct {
  /* One access unit, or the SPS/PPS with codec_config set. ts is the
   * presentation time the compositor gave the frame, in CLOCK_MONOTONIC
   * nanoseconds. The data is valid during the call only. */
  void (* data_available)(void *data, DroidMediaCodecData *encoded);
  void (* error)(void *data, int err);
  /* Called once, after the last access unit, if finish() reaches the end of
   * the stream. */
  void (* signal_eos)(void *data);
} DroidMediaScreenCaptureCallbacks;

/* Returns NULL if no hardware AVC encoder accepts the configuration, or if
 * libdroidmedia was built without screen capture (Android < 8). */
DroidMediaScreenCapture *droid_media_screen_capture_new(int width, int height, int bitrate,
                                                        int fps,
                                                        DroidMediaScreenCaptureCallbacks *cb,
                                                        void *data);

/* Starts the encoder, then publishes its input Surface to the compositor. */
bool droid_media_screen_capture_start(DroidMediaScreenCapture *capture);

/*
 * Ends the recording gracefully: withdraws the Surface from the compositor,
 * signals end of input and delivers the remaining output, then stops.
 * A non-positive timeout_ms selects the default bound. Returns true if the
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
