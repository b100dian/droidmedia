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

// A hand-written interface outside the AOSP allowlist of IInterface.h.
#define DO_NOT_CHECK_MANUAL_BINDER_INTERFACES

#include <inttypes.h>
#include <utils/Log.h>
#include <utils/Timers.h>
#include "screen_capture_service.h"

#undef LOG_TAG
#define LOG_TAG "ScreenCaptureService"

namespace android {

enum {
  GET_PRODUCER = IBinder::FIRST_CALL_TRANSACTION,
  REGISTER_PRODUCER,
  UNREGISTER_PRODUCER,
  REGISTER_LISTENER,
};

enum {
  SESSION_CHANGED = IBinder::FIRST_CALL_TRANSACTION,
};

static void writeSession(Parcel *parcel, const ScreenCaptureSession& session)
{
  parcel->writeInt64(session.generation);
  parcel->writeInt32(session.width);
  parcel->writeInt32(session.height);
  parcel->writeInt32(session.fps);
}

static ScreenCaptureSession readSession(const Parcel& parcel)
{
  ScreenCaptureSession session;
  session.generation = parcel.readInt64();
  session.width = parcel.readInt32();
  session.height = parcel.readInt32();
  session.fps = parcel.readInt32();
  return session;
}

class BpScreenCaptureSessionListener : public BpInterface<IScreenCaptureSessionListener>
{
public:
  explicit BpScreenCaptureSessionListener(const sp<IBinder>& impl) :
    BpInterface<IScreenCaptureSessionListener>(impl) {
  }

  void sessionChanged(const ScreenCaptureSession& session) override {
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureSessionListener::getInterfaceDescriptor());
    writeSession(&data, session);
    // Oneway: the service never waits for the compositor.
    status_t err = remote()->transact(SESSION_CHANGED, data, &reply, IBinder::FLAG_ONEWAY);
    if (err != NO_ERROR) {
      ALOGW("sessionChanged failed: %d", err);
    }
  }
};

IMPLEMENT_META_INTERFACE(ScreenCaptureSessionListener, "sailfish.IScreenCaptureSessionListener");

status_t BnScreenCaptureSessionListener::onTransact(uint32_t code, const Parcel& data,
                                             Parcel *reply, uint32_t flags)
{
  switch (code) {
  case SESSION_CHANGED:
    CHECK_INTERFACE(IScreenCaptureSessionListener, data, reply);
    sessionChanged(readSession(data));
    return NO_ERROR;
  default:
    return BBinder::onTransact(code, data, reply, flags);
  }
}

class BpScreenCaptureService : public BpInterface<IScreenCaptureService>
{
public:
  explicit BpScreenCaptureService(const sp<IBinder>& impl) :
    BpInterface<IScreenCaptureService>(impl) {
  }

  sp<IGraphicBufferProducer> getProducer(int *width, int *height, int *fps,
                                         int64_t *generation) override {
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    status_t err = remote()->transact(GET_PRODUCER, data, &reply);
    if (err != NO_ERROR) {
      ALOGE("getProducer failed: %d", err);
      return NULL;
    }

    sp<IGraphicBufferProducer> producer =
      interface_cast<IGraphicBufferProducer>(reply.readStrongBinder());
    *width = reply.readInt32();
    *height = reply.readInt32();
    *fps = reply.readInt32();
    *generation = reply.readInt64();
    return producer;
  }

  int64_t registerProducer(const sp<IGraphicBufferProducer>& producer,
                           int width, int height, int fps) override {
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    data.writeStrongBinder(IInterface::asBinder(producer));
    data.writeInt32(width);
    data.writeInt32(height);
    data.writeInt32(fps);
    status_t err = remote()->transact(REGISTER_PRODUCER, data, &reply);
    if (err != NO_ERROR) {
      ALOGE("registerProducer failed: %d", err);
      return 0;
    }

    return reply.readInt64();
  }

  void unregisterProducer(int64_t generation) override {
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    data.writeInt64(generation);
    status_t err = remote()->transact(UNREGISTER_PRODUCER, data, &reply);
    if (err != NO_ERROR) {
      ALOGE("unregisterProducer failed: %d", err);
    }
  }

  status_t registerListener(const sp<IScreenCaptureSessionListener>& listener,
                            ScreenCaptureSession *session) override {
    Parcel data, reply;
    data.writeInterfaceToken(IScreenCaptureService::getInterfaceDescriptor());
    data.writeStrongBinder(IInterface::asBinder(listener));
    status_t err = remote()->transact(REGISTER_LISTENER, data, &reply);
    if (err == NO_ERROR) {
      err = reply.readInt32();
    }
    if (err == NO_ERROR) {
      *session = readSession(reply);
    }
    return err;
  }
};

IMPLEMENT_META_INTERFACE(ScreenCaptureService, "sailfish.IScreenCaptureService");

status_t BnScreenCaptureService::onTransact(uint32_t code, const Parcel& data,
                                            Parcel *reply, uint32_t flags)
{
  switch (code) {
  case GET_PRODUCER: {
    CHECK_INTERFACE(IScreenCaptureService, data, reply);
    int width, height, fps;
    int64_t generation;
    sp<IGraphicBufferProducer> producer = getProducer(&width, &height, &fps, &generation);
    reply->writeStrongBinder(IInterface::asBinder(producer));
    reply->writeInt32(width);
    reply->writeInt32(height);
    reply->writeInt32(fps);
    reply->writeInt64(generation);
    return NO_ERROR;
  }
  case REGISTER_PRODUCER: {
    CHECK_INTERFACE(IScreenCaptureService, data, reply);
    sp<IGraphicBufferProducer> producer =
      interface_cast<IGraphicBufferProducer>(data.readStrongBinder());
    int width = data.readInt32();
    int height = data.readInt32();
    int fps = data.readInt32();
    reply->writeInt64(registerProducer(producer, width, height, fps));
    return NO_ERROR;
  }
  case UNREGISTER_PRODUCER:
    CHECK_INTERFACE(IScreenCaptureService, data, reply);
    unregisterProducer(data.readInt64());
    return NO_ERROR;
  case REGISTER_LISTENER: {
    CHECK_INTERFACE(IScreenCaptureService, data, reply);
    sp<IScreenCaptureSessionListener> listener =
      interface_cast<IScreenCaptureSessionListener>(data.readStrongBinder());
    ScreenCaptureSession session = {};
    status_t err = registerListener(listener, &session);
    reply->writeInt32(err);
    writeSession(reply, session);
    return NO_ERROR;
  }
  default:
    return BBinder::onTransact(code, data, reply, flags);
  }
}

// Generations start at the boot clock, so a restarted service never hands
// out one the compositor has already seen and possibly given up on.
ScreenCaptureService::ScreenCaptureService() :
  m_width(0),
  m_height(0),
  m_fps(0),
  m_generation(systemTime(SYSTEM_TIME_MONOTONIC)) {
}

sp<IGraphicBufferProducer> ScreenCaptureService::getProducer(int *width, int *height, int *fps,
                                                             int64_t *generation)
{
  Mutex::Autolock lock(m_lock);
  const ScreenCaptureSession session = currentLocked();
  *width = session.width;
  *height = session.height;
  *fps = session.fps;
  *generation = session.generation;
  return m_producer;
}

int64_t ScreenCaptureService::registerProducer(const sp<IGraphicBufferProducer>& producer,
                                               int width, int height, int fps)
{
  if (producer == NULL || width <= 0 || height <= 0 || fps <= 0) {
    ALOGE("Invalid session %dx%d @%d fps", width, height, fps);
    return 0;
  }

  Mutex::Autolock lock(m_lock);
  clearLocked();

  // This service outlives recorders: end the session if the recorder dies.
  status_t err = IInterface::asBinder(producer)->linkToDeath(this);
  if (err != NO_ERROR) {
    ALOGE("Cannot watch the recorder: %d", err);
    notifyLocked();
    return 0;
  }

  m_producer = producer;
  m_width = width;
  m_height = height;
  m_fps = fps;
  m_generation++;
  ALOGI("Session %" PRId64 " started: %dx%d @%d fps", m_generation, width, height, fps);
  notifyLocked();
  return m_generation;
}

void ScreenCaptureService::unregisterProducer(int64_t generation)
{
  Mutex::Autolock lock(m_lock);
  if (m_producer != NULL && generation == m_generation) {
    ALOGI("Session %" PRId64 " ended", generation);
    clearLocked();
    notifyLocked();
  }
}

status_t ScreenCaptureService::registerListener(const sp<IScreenCaptureSessionListener>& listener,
                                                ScreenCaptureSession *session)
{
  if (listener == NULL) {
    return BAD_VALUE;
  }

  Mutex::Autolock lock(m_lock);
  // Listeners are only removed when their process dies.
  status_t err = IInterface::asBinder(listener)->linkToDeath(this);
  if (err != NO_ERROR) {
    ALOGE("Cannot watch the listener: %d", err);
    return err;
  }

  m_listeners.push_back(listener);
  *session = currentLocked();
  ALOGI("Listener added, %zu in total", m_listeners.size());
  return NO_ERROR;
}

void ScreenCaptureService::binderDied(const wp<IBinder>& who)
{
  Mutex::Autolock lock(m_lock);
  if (m_producer != NULL && IInterface::asBinder(m_producer).get() == who.unsafe_get()) {
    ALOGW("Recorder died, session %" PRId64 " ended", m_generation);
    clearLocked();
    notifyLocked();
    return;
  }

  for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it) {
    if (IInterface::asBinder(*it).get() == who.unsafe_get()) {
      m_listeners.erase(it);
      ALOGI("Listener died, %zu left", m_listeners.size());
      return;
    }
  }
}

ScreenCaptureSession ScreenCaptureService::currentLocked() const
{
  ScreenCaptureSession session;
  session.generation = m_producer != NULL ? m_generation : 0;
  session.width = m_width;
  session.height = m_height;
  session.fps = m_fps;
  return session;
}

// Called under m_lock, so that every listener sees the changes in order:
// oneway calls to one binder are delivered in the order they were sent, and
// they never wait for the compositor.
void ScreenCaptureService::notifyLocked()
{
  const ScreenCaptureSession session = currentLocked();
  for (const auto& listener : m_listeners) {
    listener->sessionChanged(session);
  }
}

void ScreenCaptureService::clearLocked()
{
  if (m_producer != NULL) {
    IInterface::asBinder(m_producer)->unlinkToDeath(this);
    m_producer.clear();
  }
  m_width = 0;
  m_height = 0;
  m_fps = 0;
}

} // namespace android
