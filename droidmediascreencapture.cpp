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

#include <atomic>
#include <chrono>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <thread>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#include <media/MediaCodecBuffer.h>
#if ANDROID_MAJOR >= 11
#include <mediadrm/ICrypto.h>
#else
#include <media/ICrypto.h>
#endif
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
#include <utils/Timers.h>
#include "droidmediascreencapture.h"
#include "screen_capture_service.h"

#undef LOG_TAG
#define LOG_TAG "DroidMediaScreenCapture"

#define DEFAULT_FINISH_TIMEOUT_MS 2000

// The compositor learns that the session ended at once, but a frame it is
// already capturing may still be queued. After withdrawing the Surface, keep
// the codec consuming briefly, so that frame lands before input ends.
#define DETACH_GRACE_MS 100

struct _DroidMediaScreenCapture {
  enum State {
    IDLE,       // created, not started
    STARTED,    // codec running, Surface published
    FINISHING,  // finish() draining to the end of the stream
    STOPPING,   // stop() tearing down
    STOPPED,
  };

  _DroidMediaScreenCapture(int width, int height, int bitrate, int fps,
                           DroidMediaScreenCaptureCallbacks *cb, void *data) :
    m_width(width),
    m_height(height),
    m_bitrate(bitrate),
    m_fps(fps),
    m_cb_data(data),
    m_generation(0),
    m_thread_started(false),
    m_running(false),
    m_state(IDLE),
    m_eos(false),
    m_drain_exited(false),
    m_stop_requested(false) {
    if (cb) {
      m_cb = *cb;
    } else {
      memset(&m_cb, 0x0, sizeof(m_cb));
    }
  }

  ~_DroidMediaScreenCapture() {
    m_producer.clear();
    if (m_codec != NULL) {
      m_codec->release();
    }
    if (m_looper != NULL) {
      m_looper->stop();
    }
  }

  bool configure() {
    m_looper = new android::ALooper;
    m_looper->setName("DroidMediaScreenCapture");
    m_looper->start();

    android::Vector<android::AString> components;
    android::MediaCodecList::findMatchingCodecs(android::MEDIA_MIMETYPE_VIDEO_AVC, true,
                                                android::MediaCodecList::kHardwareCodecsOnly,
                                                &components);
    for (size_t i = 0; i < components.size(); i++) {
      m_codec = android::MediaCodec::CreateByComponentName(m_looper, components[i]);
      if (m_codec != NULL) {
        ALOGI("Using %s", components[i].c_str());
        break;
      }
    }
    if (m_codec == NULL) {
      ALOGE("No hardware AVC encoder");
      return false;
    }

    android::sp<android::AMessage> format = new android::AMessage;
    format->setString("mime", android::MEDIA_MIMETYPE_VIDEO_AVC);
    format->setInt32("width", m_width);
    format->setInt32("height", m_height);
    format->setInt32("frame-rate", m_fps);
    format->setInt32("bitrate", m_bitrate);
    format->setInt32("i-frame-interval", 1);
    format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
    format->setInt32("prepend-sps-pps-to-idr-frames", 1);

    android::status_t err = m_codec->configure(format, NULL, NULL,
                                               android::MediaCodec::CONFIGURE_FLAG_ENCODE);
    if (err != android::OK) {
      ALOGE("Error 0x%x configuring %dx%d @%d fps", -err, m_width, m_height, m_fps);
      return false;
    }

    err = m_codec->createInputSurface(&m_producer);
    if (err != android::OK) {
      ALOGE("Error 0x%x creating the input Surface", -err);
      return false;
    }

    return true;
  }

  bool start() {
    // The input Surface's producer end is served from this process and the
    // compositor connects to it from its own, so serve Binder here.
    android::ProcessState::self()->startThreadPool();

    android::sp<android::IBinder> binder = android::defaultServiceManager()->checkService(
      android::String16(android::ScreenCaptureService::getServiceName()));
    if (binder == NULL) {
      ALOGE("%s is not running", android::ScreenCaptureService::getServiceName());
      return false;
    }
    m_service = android::interface_cast<android::IScreenCaptureService>(binder);

    android::status_t err = m_codec->start();
    if (err != android::OK) {
      ALOGE("Error 0x%x starting codec", -err);
      m_service.clear();
      return false;
    }

    m_running = true;
    if (pthread_create(&m_thread, NULL, ThreadWrapper, this) != 0) {
      ALOGE("Cannot create the output thread");
      stopCodec();
      return false;
    }
    m_thread_started = true;

    // Publish the producer only once the codec runs: the compositor may
    // connect and queue a frame as soon as it sees the session.
    m_generation = m_service->registerProducer(m_producer, m_width, m_height, m_fps);
    if (m_generation == 0) {
      ALOGE("Cannot register the input Surface");
      stopCodec();
      return false;
    }

    android::Mutex::Autolock lock(m_lock);
    m_state = STARTED;
    ALOGI("Recording %dx%d @%d fps, session %" PRId64, m_width, m_height, m_fps, m_generation);
    return true;
  }

  // Withdraws the Surface from the compositor. Called without m_lock.
  void unregisterProducer() {
    if (m_generation == 0) {
      return;
    }

    m_service->unregisterProducer(m_generation);
    m_generation = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(DETACH_GRACE_MS));
  }

  void stopCodec() {
    m_running = false;
    // Also wakes the output thread out of dequeueOutputBuffer().
    android::status_t err = m_codec->stop();
    if (err != android::OK) {
      ALOGW("Error 0x%x stopping codec", -err);
    }
    if (m_thread_started) {
      pthread_join(m_thread, NULL);
      m_thread_started = false;
    }
    m_service.clear();
  }

  // Only the thread that moved the state to FINISHING or STOPPING calls this.
  void teardown() {
    stopCodec();

    android::Mutex::Autolock lock(m_lock);
    m_state = STOPPED;
    m_cond.broadcast();
  }

  void reportError(int err) {
    if (m_cb.error) {
      m_cb.error(m_cb_data, err);
    }
  }

  void *run() {
    bool eos = false;

    while (m_running) {
      size_t index, offset, size;
      int64_t ts;
      uint32_t flags;
      android::status_t err = m_codec->dequeueOutputBuffer(&index, &offset, &size, &ts, &flags,
                                                           100000);

      if (err == android::OK) {
        android::sp<android::MediaCodecBuffer> buffer;
        err = m_codec->getOutputBuffer(index, &buffer);
        if (err != android::OK || buffer == NULL) {
          ALOGE("Error 0x%x getting output buffer %zu", -err, index);
          m_codec->releaseOutputBuffer(index);
          reportError(err != android::OK ? err : android::UNKNOWN_ERROR);
          break;
        }

        if (size > 0 && m_cb.data_available) {
          DroidMediaCodecData data;
          data.data.data = buffer->base() + offset;
          data.data.size = size;
          data.ts = ts * 1000;
          data.decoding_ts = 0;
          data.codec_config = (flags & android::MediaCodec::BUFFER_FLAG_CODECCONFIG) != 0;
          data.sync = !data.codec_config &&
            (flags & android::MediaCodec::BUFFER_FLAG_SYNCFRAME) != 0;
          m_cb.data_available(m_cb_data, &data);
        }

        m_codec->releaseOutputBuffer(index);

        if (flags & android::MediaCodec::BUFFER_FLAG_EOS) {
          eos = true;
          if (m_cb.signal_eos) {
            m_cb.signal_eos(m_cb_data);
          }
          break;
        }
      } else if (err == android::INFO_FORMAT_CHANGED) {
        android::sp<android::AMessage> format;
        if (m_codec->getOutputFormat(&format) == android::OK) {
          ALOGI("Output format %s", format->debugString().c_str());
        }
      } else if (err == -EAGAIN || err == android::INFO_OUTPUT_BUFFERS_CHANGED) {
        continue;
      } else if (m_running) {
        ALOGE("Error 0x%x dequeueing output", -err);
        reportError(err);
        break;
      }
    }

    android::Mutex::Autolock lock(m_lock);
    m_eos = eos;
    m_drain_exited = true;
    m_cond.broadcast();
    return NULL;
  }

  static void *ThreadWrapper(void *that) {
    return static_cast<DroidMediaScreenCapture *>(that)->run();
  }

  int m_width;
  int m_height;
  int m_bitrate;
  int m_fps;
  DroidMediaScreenCaptureCallbacks m_cb;
  void *m_cb_data;
  android::sp<android::ALooper> m_looper;
  android::sp<android::MediaCodec> m_codec;
  android::sp<android::IGraphicBufferProducer> m_producer;
  android::sp<android::IScreenCaptureService> m_service;
  int64_t m_generation;
  pthread_t m_thread;
  bool m_thread_started;
  std::atomic<bool> m_running;

  // m_lock guards the members below. Callbacks are never invoked under it.
  android::Mutex m_lock;
  android::Condition m_cond;
  State m_state;
  bool m_eos;
  bool m_drain_exited;
  bool m_stop_requested;
};

extern "C" {

DroidMediaScreenCapture *droid_media_screen_capture_new(int width, int height, int bitrate,
                                                        int fps,
                                                        DroidMediaScreenCaptureCallbacks *cb,
                                                        void *data)
{
  if (width <= 0 || height <= 0 || bitrate <= 0 || fps <= 0) {
    ALOGE("Invalid configuration %dx%d @%d fps, %d bps", width, height, fps, bitrate);
    return NULL;
  }

  DroidMediaScreenCapture *capture =
    new DroidMediaScreenCapture(width, height, bitrate, fps, cb, data);
  if (!capture->configure()) {
    delete capture;
    return NULL;
  }

  return capture;
}

bool droid_media_screen_capture_start(DroidMediaScreenCapture *capture)
{
  {
    android::Mutex::Autolock lock(capture->m_lock);
    if (capture->m_state != DroidMediaScreenCapture::IDLE) {
      return false;
    }
  }

  return capture->start();
}

bool droid_media_screen_capture_finish(DroidMediaScreenCapture *capture, int timeout_ms)
{
  {
    android::Mutex::Autolock lock(capture->m_lock);
    if (capture->m_state != DroidMediaScreenCapture::STARTED) {
      return false;
    }
    capture->m_state = DroidMediaScreenCapture::FINISHING;
  }

  capture->unregisterProducer();

  // The output thread keeps running and carries the end of the stream.
  android::status_t err = capture->m_codec->signalEndOfInputStream();
  if (err != android::OK) {
    ALOGE("Error 0x%x signalling end of input", -err);
  } else {
    nsecs_t deadline = systemTime(SYSTEM_TIME_MONOTONIC) +
      ms2ns(timeout_ms > 0 ? timeout_ms : DEFAULT_FINISH_TIMEOUT_MS);
    android::Mutex::Autolock lock(capture->m_lock);
    while (!capture->m_drain_exited && !capture->m_stop_requested) {
      nsecs_t remaining = deadline - systemTime(SYSTEM_TIME_MONOTONIC);
      if (remaining <= 0) {
        break;
      }
      capture->m_cond.waitRelative(capture->m_lock, remaining);
    }
  }

  capture->teardown();

  // The output thread has been joined; m_eos is final.
  android::Mutex::Autolock lock(capture->m_lock);
  if (!capture->m_eos) {
    ALOGW("Stopped before the end of the stream");
  }
  return capture->m_eos;
}

void droid_media_screen_capture_stop(DroidMediaScreenCapture *capture)
{
  {
    android::Mutex::Autolock lock(capture->m_lock);
    switch (capture->m_state) {
    case DroidMediaScreenCapture::IDLE:
    case DroidMediaScreenCapture::STOPPED:
      return;
    case DroidMediaScreenCapture::FINISHING:
    case DroidMediaScreenCapture::STOPPING:
      // Another thread is tearing down; cut its wait short and wait for it.
      capture->m_stop_requested = true;
      capture->m_cond.broadcast();
      while (capture->m_state != DroidMediaScreenCapture::STOPPED) {
        capture->m_cond.wait(capture->m_lock);
      }
      return;
    case DroidMediaScreenCapture::STARTED:
      capture->m_state = DroidMediaScreenCapture::STOPPING;
      break;
    }
  }

  capture->unregisterProducer();
  capture->teardown();
}

void droid_media_screen_capture_destroy(DroidMediaScreenCapture *capture)
{
  droid_media_screen_capture_stop(capture);
  delete capture;
}

}
