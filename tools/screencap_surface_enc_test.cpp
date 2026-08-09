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
 * Standalone Gate-1 validation for MediaCodec Surface input.
 *
 * This deliberately does not use ScreenCaptureMediaSource, the raw input
 * encoder, or the QPA plugin. It proves only that this device can:
 *
 *   1. configure a hardware AVC encoder;
 *   2. obtain its input Surface;
 *   3. render RGBA frames through EGL into that Surface; and
 *   4. drain a non-empty, decodable H.264 stream.
 *
 * The session is run twice to expose start/stop/recreate lifecycle problems.
 */

#include <android/native_window.h>
#include <binder/ProcessState.h>
#include <media/MediaCodecBuffer.h>
#if ANDROID_MAJOR >= 11
#include <mediadrm/ICrypto.h>
#else
#include <media/ICrypto.h>
#endif
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#include <media/openmax/OMX_Video.h>
#include <utils/Log.h>
#include <utils/RefBase.h>
#include <utils/Vector.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef EGL_RECORDABLE_ANDROID
#define EGL_RECORDABLE_ANDROID 0x3142
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string>

using namespace android;

#undef LOG_TAG
#define LOG_TAG "ScreencapSurfaceTest"

namespace {

struct EglApi {
    typedef EGLBoolean (*PresentationTime)(EGLDisplay, EGLSurface,
                                            EGLnsecsANDROID);

    PresentationTime presentationTime = nullptr;

    bool load()
    {
        presentationTime = (PresentationTime) eglGetProcAddress(
            "eglPresentationTimeANDROID");
        return true;
    }
};

static GLuint compileShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512] = {};
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        fprintf(stderr, "shader compile failed: %.*s\n", (int) length, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint createBarsProgram()
{
    static const char *vertexSource =
        "attribute vec2 aPosition;\n"
        "varying vec2 vPosition;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "  vPosition = aPosition * 0.5 + 0.5;\n"
        "}\n";
    static const char *fragmentSource =
        "precision mediump float;\n"
        "varying vec2 vPosition;\n"
        "uniform float uFrame;\n"
        "void main() {\n"
        "  float x = vPosition.x;\n"
        "  if (x > 0.90 && vPosition.y < 0.10) {\n"
        "    float bit = mod(uFrame, 2.0);\n"
        "    gl_FragColor = bit < 1.0 ? vec4(0.0, 0.0, 0.0, 1.0)\n"
        "                             : vec4(1.0, 1.0, 1.0, 1.0);\n"
        "  } else if (x < 0.143) gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
        "  else if (x < 0.286) gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0);\n"
        "  else if (x < 0.429) gl_FragColor = vec4(0.0, 1.0, 1.0, 1.0);\n"
        "  else if (x < 0.572) gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
        "  else if (x < 0.715) gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n"
        "  else if (x < 0.858) gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "  else gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0);\n"
        "}\n";

    GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "aPosition");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {};
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(log), &length, log);
        fprintf(stderr, "program link failed: %.*s\n", (int) length, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static bool setupEgl(sp<Surface> surface, int width, int height,
                     EGLDisplay *outDisplay, EGLContext *outContext,
                     EGLSurface *outSurface)
{
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetDisplay failed: 0x%x\n", eglGetError());
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "eglBindAPI failed: 0x%x\n", eglGetError());
        eglTerminate(display);
        return false;
    }

    EGLint configAttrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RECORDABLE_ANDROID, EGL_TRUE,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (!eglChooseConfig(display, configAttrs, &config, 1, &configCount) ||
        configCount == 0) {
        fprintf(stderr, "eglChooseConfig failed: 0x%x\n", eglGetError());
        eglTerminate(display);
        return false;
    }

    EGLint contextAttrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLContext context = eglCreateContext(
        display, config, EGL_NO_CONTEXT, contextAttrs);
    if (context == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext failed: 0x%x\n", eglGetError());
        eglTerminate(display);
        return false;
    }

    EGLSurface eglSurface = eglCreateWindowSurface(
        display, config, static_cast<ANativeWindow *>(surface.get()), nullptr);
    if (eglSurface == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreateWindowSurface failed: 0x%x\n", eglGetError());
        eglDestroyContext(display, context);
        eglTerminate(display);
        return false;
    }

    if (!eglMakeCurrent(display, eglSurface, eglSurface, context)) {
        fprintf(stderr, "eglMakeCurrent failed: 0x%x\n", eglGetError());
        eglDestroySurface(display, eglSurface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        return false;
    }

    eglSwapInterval(display, 0);
    glViewport(0, 0, width, height);
    *outDisplay = display;
    *outContext = context;
    *outSurface = eglSurface;
    return true;
}

static void teardownEgl(EGLDisplay display, EGLContext context, EGLSurface surface)
{
    if (display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    eglTerminate(display);
}

static bool drainOutput(const sp<MediaCodec> &codec, int outputFd,
                        int expectedFrames, int *inOutAccessUnits,
                        bool waitForEos);

static int64_t monotonicNs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static void sleepUntilNs(int64_t deadlineNs)
{
    for (;;) {
        int64_t remainingNs = deadlineNs - monotonicNs();
        if (remainingNs <= 0) return;
        struct timespec delay = {
            static_cast<time_t>(remainingNs / 1000000000LL),
            static_cast<long>(remainingNs % 1000000000LL)
        };
        if (nanosleep(&delay, nullptr) == 0 || errno != EINTR) return;
    }
}

static bool renderFrames(const sp<MediaCodec> &codec, EGLDisplay display,
                         EGLSurface surface, GLuint program, int outputFd,
                         int width, int height, int frameCount,
                         int *inOutAccessUnits)
{
    static const GLfloat vertices[] = {
        -1.0f, -1.0f, 1.0f, -1.0f,
        -1.0f,  1.0f, 1.0f,  1.0f,
    };

    glUseProgram(program);
    GLint position = 0;
    glEnableVertexAttribArray(position);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, vertices);

    EglApi eglApi;
    eglApi.load();
    GLint frameUniform = glGetUniformLocation(program, "uFrame");
    const int64_t startNs = monotonicNs();
    const int64_t framePeriodNs = 1000000000LL / 30;
    for (int frame = 0; frame < frameCount; ++frame) {
        sleepUntilNs(startNs + frame * framePeriodNs);
        glUniform1f(frameUniform, static_cast<GLfloat>(frame));
        glViewport(0, 0, width, height);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFlush();

        if (eglApi.presentationTime) {
            eglApi.presentationTime(display, surface,
                                    static_cast<EGLnsecsANDROID>(
                                        frame * 1000000000LL / 30));
        }
        if (!eglSwapBuffers(display, surface)) {
            fprintf(stderr, "eglSwapBuffers failed at frame %d: 0x%x\n",
                    frame, eglGetError());
            glDisableVertexAttribArray(position);
            return false;
        }

        // Keep the codec's output queue moving while the input Surface is
        // being fed. Without this, eglSwapBuffers() can block once the
        // encoder runs out of output slots.
        if (!drainOutput(codec, outputFd, 0, inOutAccessUnits, false)) {
            glDisableVertexAttribArray(position);
            return false;
        }
    }

    glDisableVertexAttribArray(position);
    return true;
}

static bool drainOutput(const sp<MediaCodec> &codec, int outputFd,
                        int expectedFrames, int *inOutAccessUnits,
                        bool waitForEos)
{
    int accessUnits = inOutAccessUnits ? *inOutAccessUnits : 0;
    int idlePolls = 0;
    bool eos = false;

    while (!eos && (waitForEos || idlePolls == 0)) {
        size_t index = 0;
        size_t offset = 0;
        size_t size = 0;
        int64_t timeUs = 0;
        uint32_t flags = 0;
        status_t err = codec->dequeueOutputBuffer(
            &index, &offset, &size, &timeUs, &flags,
            waitForEos ? 500000LL : 0LL);

        if (err == OK) {
            idlePolls = 0;
            sp<MediaCodecBuffer> output;
            status_t bufferErr = codec->getOutputBuffer(index, &output);
            if (bufferErr != OK || output == nullptr) {
                fprintf(stderr, "getOutputBuffer(%zu) failed: %d\n",
                        index, bufferErr);
                codec->releaseOutputBuffer(index);
                return false;
            }

            fprintf(stderr, "output: index=%zu size=%zu pts=%" PRId64
                    " flags=0x%x%s\n", index, size, timeUs, flags,
                    (flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) ?
                        " codec-config" : "");

            if (size > 0) {
                const uint8_t *data = output->base() + offset;
                ssize_t written = write(outputFd, data, size);
                if (written != static_cast<ssize_t>(size)) {
                    fprintf(stderr, "write output failed: %s\n", strerror(errno));
                    codec->releaseOutputBuffer(index);
                    return false;
                }
                if (!(flags & MediaCodec::BUFFER_FLAG_CODECCONFIG)) {
                    ++accessUnits;
                }
            }

            if (flags & MediaCodec::BUFFER_FLAG_EOS) {
                eos = true;
            }
            codec->releaseOutputBuffer(index);
        } else if (err == INFO_FORMAT_CHANGED) {
            sp<AMessage> format;
            if (codec->getOutputFormat(&format) == OK && format != nullptr) {
                AString mime;
                if (format->findString("mime", &mime)) {
                    fprintf(stderr, "output format: %s\n", mime.c_str());
                }
            }
            idlePolls = 0;
        } else if (err == -EAGAIN) {
            if (!waitForEos) break;
            ++idlePolls;
        } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
            idlePolls = 0;
        } else {
            fprintf(stderr, "dequeueOutputBuffer returned %d\n", err);
            return false;
        }
    }

    if (inOutAccessUnits) *inOutAccessUnits = accessUnits;
    if (!waitForEos) return true;

    if (!eos) {
        fprintf(stderr, "encoder did not signal EOS after %d output units\n",
                accessUnits);
        return false;
    }
    if (accessUnits == 0) {
        fprintf(stderr, "encoder produced no non-config output units\n");
        return false;
    }
    fprintf(stderr, "encoder produced %d non-config output buffers for %d submitted frames\n",
            accessUnits, expectedFrames);
    return true;
}

static bool runSession(int width, int height, int frameCount,
                       const char *outputPath, int session)
{
    fprintf(stderr, "session %d: configuring AVC Surface encoder\n", session);

    sp<ALooper> looper = new ALooper;
    looper->setName("ScreencapSurfaceTestLooper");
    looper->start();

    Vector<AString> components;
    MediaCodecList::findMatchingCodecs(
        MEDIA_MIMETYPE_VIDEO_AVC, true /* encoder */,
        MediaCodecList::kHardwareCodecsOnly, &components);

    sp<MediaCodec> codec;
    AString componentName;
    for (size_t i = 0; i < components.size(); ++i) {
        codec = MediaCodec::CreateByComponentName(looper, components[i]);
        if (codec != nullptr) {
            componentName = components[i];
            break;
        }
    }
    if (codec == nullptr) {
        fprintf(stderr, "session %d: no hardware AVC encoder component found\n",
                session);
        looper->stop();
        return false;
    }
    fprintf(stderr, "session %d: using hardware component %s\n",
            session, componentName.c_str());

    sp<AMessage> format = new AMessage;
    format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
    format->setInt32("width", width);
    format->setInt32("height", height);
    format->setInt32("frame-rate", 30);
    format->setInt32("bitrate", 8000000);
    format->setInt32("i-frame-interval", 1);
    format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
    format->setInt32("prepend-sps-pps-to-idr-frames", 1);

    status_t err = codec->configure(
        format, nullptr, nullptr, MediaCodec::CONFIGURE_FLAG_ENCODE);
    if (err != OK) {
        fprintf(stderr, "session %d: configure failed: %d\n", session, err);
        codec->release();
        looper->stop();
        return false;
    }

    sp<IGraphicBufferProducer> producer;
    err = codec->createInputSurface(&producer);
    if (err != OK || producer == nullptr) {
        fprintf(stderr, "session %d: createInputSurface failed: %d\n", session, err);
        codec->release();
        looper->stop();
        return false;
    }

    sp<Surface> inputSurface = new Surface(producer, true);
    if (inputSurface == nullptr) {
        fprintf(stderr, "session %d: Surface construction failed\n", session);
        producer.clear();
        codec->release();
        looper->stop();
        return false;
    }

    err = codec->start();
    if (err != OK) {
        fprintf(stderr, "session %d: codec start failed: %d\n", session, err);
        inputSurface.clear();
        producer.clear();
        codec->release();
        looper->stop();
        return false;
    }

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    if (!setupEgl(inputSurface, width, height, &display, &context, &surface)) {
        codec->stop();
        inputSurface.clear();
        producer.clear();
        codec->release();
        looper->stop();
        return false;
    }

    GLuint program = createBarsProgram();
    if (!program) {
        teardownEgl(display, context, surface);
        inputSurface.clear();
        producer.clear();
        codec->release();
        looper->stop();
        return false;
    }

    int outputFd = open(outputPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outputFd < 0) {
        fprintf(stderr, "session %d: cannot open %s: %s\n",
                session, outputPath, strerror(errno));
        codec->stop();
        glDeleteProgram(program);
        teardownEgl(display, context, surface);
        inputSurface.clear();
        producer.clear();
        codec->release();
        looper->stop();
        return false;
    }

    fprintf(stderr, "session %d: rendering %d frames to %s\n",
            session, frameCount, outputPath);
    int accessUnits = 0;
    bool rendered = renderFrames(codec, display, surface, program, outputFd,
                                 width, height, frameCount, &accessUnits);
    if (!rendered) {
        close(outputFd);
        codec->stop();
        glDeleteProgram(program);
        teardownEgl(display, context, surface);
        inputSurface.clear();
        producer.clear();
        codec->release();
        looper->stop();
        return false;
    }

    err = codec->signalEndOfInputStream();
    if (err != OK) {
        fprintf(stderr, "session %d: signalEndOfInputStream failed: %d\n",
                session, err);
        close(outputFd);
        codec->stop();
        glDeleteProgram(program);
        teardownEgl(display, context, surface);
        inputSurface.clear();
        producer.clear();
        codec->release();
        looper->stop();
        return false;
    }

    bool drained = drainOutput(codec, outputFd, frameCount, &accessUnits,
                               true);
    close(outputFd);

    status_t stopErr = codec->stop();
    glDeleteProgram(program);
    teardownEgl(display, context, surface);
    inputSurface.clear();
    producer.clear();
    status_t releaseErr = codec->release();
    looper->stop();

    struct stat outputStat = {};
    bool outputExists = stat(outputPath, &outputStat) == 0;
    fprintf(stderr, "session %d: output units=%d bytes=%ld stop=%d release=%d\n",
            session, accessUnits,
            outputExists ? static_cast<long>(outputStat.st_size) : 0L,
            stopErr, releaseErr);

    return drained && stopErr == OK && releaseErr == OK && outputExists &&
           outputStat.st_size > 0;
}

} // namespace

int main(int argc, char **argv)
{
    int width = argc > 1 ? atoi(argv[1]) : 1280;
    int height = argc > 2 ? atoi(argv[2]) : 720;
    int frameCount = argc > 3 ? atoi(argv[3]) : 60;
    const char *outputPath = argc > 4 ? argv[4] : "/tmp/screencap-surface.h264";

    if (width <= 0 || height <= 0 || frameCount < 2) {
        fprintf(stderr, "usage: %s [width] [height] [frames] [output.h264]\n",
                argv[0]);
        return 2;
    }

    std::string restartPath = std::string(outputPath) + ".restart";
    fprintf(stderr, "Surface-input AVC test: %dx%d, %d frames\n",
            width, height, frameCount);

    ProcessState::self()->startThreadPool();
    bool first = runSession(width, height, frameCount, outputPath, 1);
    bool second = runSession(width, height, frameCount, restartPath.c_str(), 2);
    if (first && second) {
        fprintf(stderr, "=== SURFACE INPUT TEST PASSED ===\n");
        fprintf(stderr, "validate with:\n");
        fprintf(stderr, "  ffprobe -v error -show_streams %s\n", outputPath);
        fprintf(stderr, "  ffmpeg -v error -i %s -frames:v 1 /tmp/screencap-surface.png\n",
                outputPath);
        fprintf(stderr, "  ffprobe -v error -show_streams %s\n", restartPath.c_str());
        return 0;
    }

    fprintf(stderr, "=== SURFACE INPUT TEST FAILED ===\n");
    return 1;
}
