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

/*
 * Screen capture entry points for the compositor. qt5-qpa-hwcomposer-plugin
 * resolves them with android_dlsym() and renders the display into the
 * Surface of the recorder registered with sailfish.screencap. They are called
 * on the compositor's render thread and must not block on a missing service.
 */

#include <inttypes.h>
#include <string.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <gui/Surface.h>
#include <utils/Log.h>
#include <utils/Mutex.h>
#include "screen_capture_service.h"

#undef LOG_TAG
#define LOG_TAG "MinisfScreenCapture"

using namespace android;

/* qt5-qpa-hwcomposer-plugin declares the same layout; keep them in sync. */
typedef struct {
  uint64_t generation;
  int width;
  int height;
  int fps;
} MinisfScreenCaptureSessionInfo;

struct MinisfScreenCaptureTarget {
  sp<Surface> surface;
  uint64_t generation;
};

static Mutex s_lock;
static sp<IScreenCaptureService> s_service;

static sp<IScreenCaptureService> screenCaptureService()
{
  Mutex::Autolock lock(s_lock);

  if (s_service != NULL && IInterface::asBinder(s_service)->isBinderAlive()) {
    return s_service;
  }

  // The encoder's queue calls the producer listener that Surface serves from
  // this process, so this process needs Binder threads.
  ProcessState::self()->startThreadPool();

  // Never getService(): it waits up to 5 s for a missing service.
  sp<IBinder> binder =
    defaultServiceManager()->checkService(String16(ScreenCaptureService::getServiceName()));
  s_service = interface_cast<IScreenCaptureService>(binder);
  return s_service;
}

extern "C" {

int minisf_screen_capture_session_query(MinisfScreenCaptureSessionInfo *info)
{
  memset(info, 0, sizeof(*info));

  sp<IScreenCaptureService> service = screenCaptureService();
  if (service == NULL) {
    return 0;
  }

  int64_t generation = 0;
  if (service->getProducer(&info->width, &info->height, &info->fps, &generation) == NULL) {
    memset(info, 0, sizeof(*info));
    return 0;
  }

  info->generation = generation;
  return 1;
}

void *minisf_screen_capture_target_acquire_generation(uint64_t generation)
{
  sp<IScreenCaptureService> service = screenCaptureService();
  if (service == NULL) {
    return NULL;
  }

  int width, height, fps;
  int64_t current = 0;
  sp<IGraphicBufferProducer> producer = service->getProducer(&width, &height, &fps, &current);
  if (producer == NULL || (uint64_t)current != generation) {
    return NULL;
  }

  MinisfScreenCaptureTarget *target = new MinisfScreenCaptureTarget;
  target->surface = new Surface(producer, true);
  target->generation = generation;
  ALOGI("Attached to session %" PRIu64, generation);
  return target;
}

void *minisf_screen_capture_target_native_window(void *target)
{
  if (!target) {
    return NULL;
  }

  // RefBase, not ANativeWindow, is Surface's primary base: convert to the
  // ANativeWindow subobject before the pointer loses its type.
  ANativeWindow *window = static_cast<MinisfScreenCaptureTarget *>(target)->surface.get();
  return window;
}

void minisf_screen_capture_target_release(void *target)
{
  MinisfScreenCaptureTarget *t = static_cast<MinisfScreenCaptureTarget *>(target);
  if (t) {
    ALOGI("Detached from session %" PRIu64, t->generation);
    delete t;
  }
}

}
