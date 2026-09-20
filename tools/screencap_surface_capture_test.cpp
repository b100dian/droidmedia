/*
 * Gate-2 recorder-side interoperability test.
 *
 * Creates the same Surface-input AVC encoder used by the future recorder,
 * registers its producer with sailfish.screencap, and drains H.264 while the
 * QPA process renders deterministic test bars into the registered Surface.
 * The test intentionally does not create an EGL context: QPA is the producer.
 */

#include "../screen_capture_surface_encoder.h"

#include <media/stagefright/MediaCodec.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

struct TestState {
    int outputFd;
    int frames;
    bool error;
};

static void onData(void *opaque, const ScreenCaptureSurfaceEncodedFrame *frame)
{
    TestState *state = static_cast<TestState *>(opaque);
    if (frame->size == 0) return;
    ssize_t written = write(state->outputFd, frame->data, frame->size);
    if (written != static_cast<ssize_t>(frame->size)) {
        fprintf(stderr, "write H.264 failed: %s\n", strerror(errno));
        state->error = true;
        return;
    }
    if (!(frame->flags & android::MediaCodec::BUFFER_FLAG_CODECCONFIG)) {
        ++state->frames;
    }
}

static void onFormat(void *, int width, int height)
{
    fprintf(stderr, "output format: %dx%d\n", width, height);
}

static void onError(void *opaque, int error)
{
    TestState *state = static_cast<TestState *>(opaque);
    state->error = true;
    fprintf(stderr, "encoder error: %d\n", error);
}

static void onEos(void *)
{
    fprintf(stderr, "encoder EOS\n");
}

} // namespace

int main(int argc, char **argv)
{
    const int width = argc > 1 ? atoi(argv[1]) : 1280;
    const int height = argc > 2 ? atoi(argv[2]) : 720;
    const int bitrate = argc > 3 ? atoi(argv[3]) : 8000000;
    const int fps = argc > 4 ? atoi(argv[4]) : 30;
    const int duration = argc > 5 ? atoi(argv[5]) : 10;
    const char *path = argc > 6 ? argv[6] : "/tmp/screencap-gate2.h264";

    if (width <= 0 || height <= 0 || bitrate <= 0 || fps <= 0 || duration <= 0) {
        fprintf(stderr, "usage: %s [width] [height] [bitrate] [fps] [seconds] [output]\n",
                argv[0]);
        return 2;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }

    TestState state = {fd, 0, false};
    ScreenCaptureSurfaceEncoderCallbacks callbacks = {};
    callbacks.data_available = onData;
    callbacks.format_changed = onFormat;
    callbacks.error = onError;
    callbacks.eos = onEos;

    fprintf(stderr, "Gate-2 Surface capture test: %dx%d @%d fps for %d seconds\n",
            width, height, fps, duration);
    ScreenCaptureSurfaceEncoder *encoder =
        screen_capture_surface_encoder_new(width, height, bitrate, fps,
                                           &callbacks, &state);
    if (!encoder || !screen_capture_surface_encoder_start(encoder)) {
        fprintf(stderr, "failed to start Surface encoder or register producer\n");
        if (encoder) screen_capture_surface_encoder_destroy(encoder);
        close(fd);
        return 1;
    }

    fprintf(stderr, "producer registered; encoder and output drain are ready\n");
    fprintf(stderr, "for Gate 2.1/2.1a, keep QPA capture disabled and start the "
            "standalone EGL producer\n");
    fprintf(stderr, "waiting %d seconds for encoder-Surface frames...\n", duration);
    sleep(static_cast<unsigned int>(duration));

    screen_capture_surface_encoder_stop(encoder);
    screen_capture_surface_encoder_destroy(encoder);
    close(fd);

    fprintf(stderr, "Gate-2 recorder stopped: frames=%d error=%d output=%s\n",
            state.frames, state.error ? 1 : 0, path);
    return state.error || state.frames == 0 ? 1 : 0;
}
