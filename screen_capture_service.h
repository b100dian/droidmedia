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

#ifndef SCREEN_CAPTURE_SERVICE_H
#define SCREEN_CAPTURE_SERVICE_H

#include <binder/BinderService.h>
#include <binder/IInterface.h>
#include <gui/IGraphicBufferProducer.h>
#include <utils/Mutex.h>

namespace android {

/*
 * sailfish.screencap pairs a screen recorder with the compositor.
 *
 * The recorder (droidmediascreencapture.cpp) registers the input Surface
 * producer of its hardware encoder. The compositor polls getProducer()
 * through libminisf (minisf_screen_capture.cpp) and renders the display into
 * that Surface. There is one session at a time; every registration starts a
 * new generation, which tells the compositor that its current target is stale.
 */
class IScreenCaptureService : public IInterface
{
public:
  DECLARE_META_INTERFACE(ScreenCaptureService);

  /* Returns NULL when no recorder is registered. */
  virtual sp<IGraphicBufferProducer> getProducer(int *width, int *height, int *fps,
                                                 int64_t *generation) = 0;
  /* Replaces any current session. Returns its generation, or 0 on failure. */
  virtual int64_t registerProducer(const sp<IGraphicBufferProducer>& producer,
                                   int width, int height, int fps) = 0;
  /* Ends the session if it is still the given generation. */
  virtual void unregisterProducer(int64_t generation) = 0;
};

class BnScreenCaptureService : public BnInterface<IScreenCaptureService>
{
public:
  status_t onTransact(uint32_t code, const Parcel& data, Parcel *reply,
                      uint32_t flags = 0) override;
};

/* Hosted by minisfservice. */
class ScreenCaptureService : public BinderService<ScreenCaptureService>,
                             public BnScreenCaptureService,
                             public IBinder::DeathRecipient
{
public:
  static char const *getServiceName() { return "sailfish.screencap"; }

  ScreenCaptureService();

  sp<IGraphicBufferProducer> getProducer(int *width, int *height, int *fps,
                                         int64_t *generation) override;
  int64_t registerProducer(const sp<IGraphicBufferProducer>& producer,
                           int width, int height, int fps) override;
  void unregisterProducer(int64_t generation) override;

  void binderDied(const wp<IBinder>& who) override;

private:
  void clearLocked();

  Mutex m_lock;
  sp<IGraphicBufferProducer> m_producer;
  int m_width;
  int m_height;
  int m_fps;
  int64_t m_generation;
};

} // namespace android

#endif /* SCREEN_CAPTURE_SERVICE_H */
