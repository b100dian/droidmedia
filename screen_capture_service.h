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

#include <vector>

namespace sailfish {

/* qt5-qpa-hwcomposer-plugin declares the same layout; keep them in sync. */
struct ScreenCaptureSession {
  int64_t generation;  // 0: no session
  int width;
  int height;
  int fps;
};

/* Implemented by the compositor (through libminisf). Calls are oneway. */
class IScreenCaptureSessionListener : public android::IInterface
{
public:
  DECLARE_META_INTERFACE(ScreenCaptureSessionListener);

  virtual void sessionChanged(const ScreenCaptureSession& session) = 0;
};

class BnScreenCaptureSessionListener :
  public android::BnInterface<IScreenCaptureSessionListener>
{
public:
  android::status_t onTransact(uint32_t code, const android::Parcel& data,
                               android::Parcel *reply, uint32_t flags = 0) override;
};

/*
 * sailfish.screencap service pairs a screen recorder with the compositor.
 *
 * The recorder (droidmediascreencapture.cpp) registers the input Surface producer
 * of its hardware encoder. The compositor subscribes through libminisf
 * (minisf_screen_capture.cpp), and is notified of every session change and
 * renders the display into that Surface. There is one session at a time.
 */
class IScreenCaptureService : public android::IInterface
{
public:
  DECLARE_META_INTERFACE(ScreenCaptureService);

  /* Returns NULL when no recorder is registered. */
  virtual android::sp<android::IGraphicBufferProducer> getProducer(int *width, int *height,
                                                                   int *fps,
                                                                   int64_t *generation) = 0;
  /* Replaces any current session. Returns its generation, or 0 on failure. */
  virtual int64_t registerProducer(const android::sp<android::IGraphicBufferProducer>& producer,
                                   int width, int height, int fps) = 0;
  /* Ends the session if it is still the given generation. */
  virtual void unregisterProducer(int64_t generation) = 0;
  /* Calls listener->sessionChanged(). Returns the current session in *session. */
  virtual android::status_t registerListener(
    const android::sp<IScreenCaptureSessionListener>& listener,
    ScreenCaptureSession *session) = 0;
};

class BnScreenCaptureService : public android::BnInterface<IScreenCaptureService>
{
public:
  android::status_t onTransact(uint32_t code, const android::Parcel& data,
                               android::Parcel *reply, uint32_t flags = 0) override;
};

/* Hosted by minisfservice. */
class ScreenCaptureService : public android::BinderService<ScreenCaptureService>,
                             public BnScreenCaptureService,
                             public android::IBinder::DeathRecipient
{
public:
  static char const *getServiceName() { return "sailfish.screencap"; }

  ScreenCaptureService();

  android::sp<android::IGraphicBufferProducer> getProducer(int *width, int *height, int *fps,
                                                           int64_t *generation) override;
  int64_t registerProducer(const android::sp<android::IGraphicBufferProducer>& producer,
                           int width, int height, int fps) override;
  void unregisterProducer(int64_t generation) override;
  android::status_t registerListener(const android::sp<IScreenCaptureSessionListener>& listener,
                                     ScreenCaptureSession *session) override;

  void binderDied(const android::wp<android::IBinder>& who) override;

private:
  // *Locked() methods require m_lock.
  ScreenCaptureSession sessionLocked() const;
  void clearLocked();
  void notifyLocked();

  // m_lock guards the members below.
  android::Mutex m_lock;
  android::sp<android::IGraphicBufferProducer> m_producer;
  int m_width;
  int m_height;
  int m_fps;
  int64_t m_generation;
  std::vector<android::sp<IScreenCaptureSessionListener>> m_listeners;
};

} // namespace sailfish

#endif /* SCREEN_CAPTURE_SERVICE_H */
