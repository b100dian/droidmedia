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
 * screencap_enc_test.cpp — Phase-0 standalone validation harness.
 *
 * Allocates a single RGBA GraphicBuffer, fills it with color bars,
 * and feeds it to the hardware H.264 encoder.
 *
 * Two paths are attempted:
 *   A) AndroidOpaque + meta_data=1 (zero-copy / metadata mode — preferred)
 *   B) AndroidOpaque + meta_data=0 (copy RGBA into encoder input — fallback)
 *
 * On some Codec2/A14 stacks, path A is rejected even though the format
 * is listed as supported. Path B is the v1 target — the encoder does RGB→YUV
 * conversion internally (AndroidOpaque semantics). MediaBuffer ownership
 * follows AsyncCodecSource: the source buffer is copied synchronously and
 * released by the reader after queueing.
 *
 * Uses the same droid_media_codec_create_encoder_raw + MediaSource
 * poll pattern as DroidMediaRecorder (droidmediarecorder.cpp).
 *
 * Usage (on device, in Android namespace):
 *   screencap_enc_test [width] [height] [num_frames] [output_file]
 *   Defaults: 1920 1080 30 /tmp/screencap_test.h264
 */

#include "droidmedia.h"
#include "droidmediacodec.h"
#include <OMX_Video.h>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <binder/ProcessState.h>
#include <utils/RefBase.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/hardware/MetadataBufferType.h>
#include <ui/GraphicBuffer.h>

// Forward-declare droid_media_codec_create_encoder_raw() — pulling in
// private.h would drag camera/Camera.h and other heavy deps.
android::sp<android::MediaSource>
droid_media_codec_create_encoder_raw(
    DroidMediaCodecEncoderMetaData *meta,
    android::sp<android::ALooper> looper,
    android::sp<android::MediaSource> src);

using namespace android;

#undef LOG_TAG
#define LOG_TAG "ScreencapTest"

/* ------------------------------------------------------------------ */
/* Test MediaSource.
 *
 * Two modes:
 *   metadata=true  — write VideoGrallocMetadata referencing the handle
 *                    (zero-copy, may be rejected by Codec2)
 *   metadata=false — lock the GraphicBuffer, memcpy RGBA into the
 *                    encoder's input buffer (one copy, fast path) */
/* ------------------------------------------------------------------ */
class TestMediaSource : public MediaSource {
public:
    TestMediaSource(sp<GraphicBuffer> gb, int w, int h, int nFrames,
                    int colorFormat, bool metadataMode)
        : mGB(gb), mW(w), mH(h), mNum(nFrames),
          mCount(0), mColorFormat(colorFormat),
          mMetadataMode(metadataMode), mStarted(false) {}

    status_t start(MetaData*) override { mStarted = true; return OK; }
    status_t stop() override { mStarted = false; return OK; }

    sp<MetaData> getFormat() override {
        sp<MetaData> md = new MetaData;
        md->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RAW);
        md->setInt32(kKeyWidth, mW);
        md->setInt32(kKeyHeight, mH);
        md->setInt32(kKeyColorFormat, mColorFormat);
        // max-input-size: RGBA frame size
        md->setInt32(kKeyMaxInputSize, mW * mH * 4);
        md->setInt32(kKeyStride, mW);
        md->setInt32(kKeySliceHeight, mH);
        return md;
    }

    status_t read(MediaBufferBase **out,
                  const ReadOptions* = nullptr) override {
        if (mCount >= mNum) {
            fprintf(stderr, "  source: EOS after %d frames\n", mCount);
            return ERROR_END_OF_STREAM;
        }

        if (mMetadataMode) {
            // Zero-copy metadata path: just the handle + type tag
            struct GrallocMeta { int32_t eType; buffer_handle_t h; };
            GrallocMeta meta;
            meta.eType = kMetadataBufferTypeGrallocSource;
            meta.h     = mGB->handle;

            MediaBuffer *mb = new MediaBuffer(sizeof(meta));
            memcpy(mb->data(), &meta, sizeof(meta));
            mb->set_range(0, sizeof(meta));
#if ANDROID_MAJOR >= 9
            mb->meta_data().setInt64(kKeyTime, mCount * 33333LL);
#else
            mb->meta_data()->setInt64(kKeyTime, mCount * 33333LL);
#endif
            mCount++;
            *out = mb;
            return OK;
        }

        // Non-metadata path: copy real RGBA pixel data from the
        // GraphicBuffer into the encoder's input buffer.
        // The encoder handles RGB→YUV conversion internally
        // (this is what AndroidOpaque without FLAG_USE_METADATA means).

        uint8_t *src = nullptr;
        status_t lerr = mGB->lock(GRALLOC_USAGE_SW_READ_OFTEN, (void**)&src);
        if (lerr != OK || !src) {
            fprintf(stderr, "  source: lock failed: %d\n", lerr);
            return ERROR_END_OF_STREAM;
        }

        size_t frameSize = mW * mH * 4;
        MediaBuffer *mb = new MediaBuffer(frameSize);
        int stride = mGB->getStride();

        // Copy row by row (stride may differ from width*4)
        uint8_t *dst = (uint8_t*)mb->data();
        for (int y = 0; y < mH; y++) {
            memcpy(dst + y * mW * 4, src + y * stride * 4, mW * 4);
        }

        mGB->unlock();
        mb->set_range(0, frameSize);
#if ANDROID_MAJOR >= 9
        mb->meta_data().setInt64(kKeyTime, mCount * 33333LL);
#else
        mb->meta_data()->setInt64(kKeyTime, mCount * 33333LL);
#endif

        mCount++;
        *out = mb;
        return OK;
    }

private:
    sp<GraphicBuffer> mGB;
    int mW, mH, mNum, mCount, mColorFormat;
    bool mMetadataMode;
    bool mStarted;
};

/* ------------------------------------------------------------------ */
/* Main */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    int w = argc > 1 ? atoi(argv[1]) : 1920;
    int h = argc > 2 ? atoi(argv[2]) : 1080;
    int N = argc > 3 ? atoi(argv[3]) : 30;
    const char *out = argc > 4 ? argv[4] : "/tmp/screencap_test.h264";

    fprintf(stderr, "=== HWC H.264 Phase-0 Validation Harness ===\n");
    fprintf(stderr, "    %dx%d, %d frames -> %s\n", w, h, N, out);

    // 1. Init
    ProcessState::self()->startThreadPool();
    fprintf(stderr, "[1/7] ProcessState initialized\n");

    // 2. Allocate a single RGBA GraphicBuffer (no BufferQueue needed)
    sp<GraphicBuffer> gb = new GraphicBuffer(
        w, h, HAL_PIXEL_FORMAT_RGBA_8888, 1,
        GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_VIDEO_ENCODER |
        GRALLOC_USAGE_SW_WRITE_OFTEN,
        std::string("ScreencapTest"));
    if (gb == NULL || gb->handle == NULL) {
        fprintf(stderr, "FATAL: GraphicBuffer allocation failed\n");
        return 1;
    }
    fprintf(stderr, "[2/7] GraphicBuffer allocated: %dx%d stride=%d handle=%p\n",
            w, h, gb->getStride(), gb->handle);

    // 3. Fill with color bars
    uint8_t *p = nullptr;
    status_t lerr = gb->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)&p);
    if (lerr == OK && p) {
        int stride = gb->getStride();
        for (int y = 0; y < h; y++) {
            uint8_t *row = p + y * stride * 4;
            uint8_t r = (uint8_t)((y * 255 / h) & 0xFF);
            uint8_t g = (uint8_t)(((y + 85) * 255 / h) & 0xFF);
            uint8_t b_ = (uint8_t)(((y + 170) * 255 / h) & 0xFF);
            for (int x = 0; x < w; x++) {
                row[x*4+0]=r; row[x*4+1]=g; row[x*4+2]=b_; row[x*4+3]=255;
            }
        }
        gb->unlock();
    }
    fprintf(stderr, "[3/7] color bars written to GraphicBuffer\n");

    // 4. Probe — what color formats does the HW AVC encoder actually support?
    uint32_t formats[32];
    DroidMediaCodecMetaData probe = {};
    probe.type   = "video/avc";
    probe.width  = w;
    probe.height = h;
    probe.fps    = 30;
    probe.flags  = DROID_MEDIA_CODEC_HW_ONLY;
    unsigned int nf = droid_media_codec_get_supported_color_formats(
        &probe, 1 /* encoder */, formats, 32);
    fprintf(stderr, "[3.5] HW encoder color formats (%u):\n", nf);
    for (unsigned int i = 0; i < nf && i < 32; i++) {
        const char *tag = "";
        if (formats[i] == 0x7F000789) tag = " (AndroidOpaque)";
        if (formats[i] == 0x7FA30C04) tag = " (QCOM NV12 Tiled)";
        if (formats[i] == 21)         tag = " (NV12)";
        if (formats[i] == 19)         tag = " (YV12)";
        fprintf(stderr, "      0x%08x%s\n", formats[i], tag);
    }

    // 5. Build encoder meta — try metadata mode first, fall back to copy
    DroidMediaCodecEncoderMetaData md = {};
    md.parent.type   = "video/avc";
    md.parent.width  = w;
    md.parent.height = h;
    md.parent.fps    = 30;
    md.parent.flags  = DROID_MEDIA_CODEC_HW_ONLY;
    md.color_format  = 0x7F000789;  // AndroidOpaque
    md.bitrate       = 8000000;
    md.stride        = w;
    md.slice_height  = h;

    sp<ALooper> looper = new ALooper;
    looper->setName("ScreencapLooper");
    looper->start();

    sp<MediaSource> codec = NULL;
    bool metadataMode = false;

    // Attempt A: metadata mode (zero-copy, preferred)
    md.meta_data = 1;
    sp<TestMediaSource> srcA = new TestMediaSource(gb, w, h, N, md.color_format, true);
    codec = droid_media_codec_create_encoder_raw(&md, looper, srcA);
    fprintf(stderr, "  Attempt A (AndroidOpaque + metadata): %s\n",
            codec != NULL ? "SUCCESS" : "FAILED");
    if (codec != NULL) metadataMode = true;

    // Attempt B: non-metadata mode — copy RGBA into encoder (robust fallback)
    if (codec == NULL) {
        md.meta_data     = 0;
        md.max_input_size = w * h * 4;  // RGBA frame size
        sp<TestMediaSource> srcB = new TestMediaSource(gb, w, h, N, md.color_format, false);
        codec = droid_media_codec_create_encoder_raw(&md, looper, srcB);
        fprintf(stderr, "  Attempt B (AndroidOpaque + copy RGBA): %s\n",
                codec != NULL ? "SUCCESS" : "FAILED");
    }

    if (codec == NULL) {
        fprintf(stderr, "FATAL: All encoder creation attempts failed.\n");
        return 1;
    }

    // 6. Start
    status_t err = codec->start();
    if (err != OK) {
        fprintf(stderr, "FATAL: codec start failed: %d\n", err);
        return 1;
    }
    fprintf(stderr, "[4/7] encoder started (mode=%s)\n",
            metadataMode ? "metadata" : "copy");

    // 7. Poll loop
    int outFd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) {
        fprintf(stderr, "FATAL: cannot open '%s': %s\n", out, strerror(errno));
        return 1;
    }

    int framesOut = 0;
    int stalled = 0;
    fprintf(stderr, "[5/7] poll loop running, expect %d frames...\n", N);
    while (framesOut < N) {
        MediaBuffer *buffer = NULL;
#if ANDROID_MAJOR >= 9
        err = codec->read((MediaBufferBase**)&buffer);
#else
        err = codec->read(&buffer);
#endif
        if (err == OK && buffer) {
            // Write encoded NAL unit to file
            if (outFd >= 0) {
                ssize_t written = write(outFd,
                    (uint8_t*)buffer->data() + buffer->range_offset(),
                    buffer->range_length());
                (void)written;
            }
            int32_t sync = 0;
#if ANDROID_MAJOR >= 9
            buffer->meta_data().findInt32(kKeyIsSyncFrame, &sync);
#else
            buffer->meta_data()->findInt32(kKeyIsSyncFrame, &sync);
#endif
            bool isSync = (sync != 0);
            framesOut++;
            if (framesOut % 10 == 0 || isSync) {
                fprintf(stderr, "  frame %d/%d  size=%zu  %s\n",
                        framesOut, N, buffer->range_length(),
                        isSync ? "IDR" : "P");
            }
            buffer->release();
            stalled = 0;
        } else if (err == INFO_FORMAT_CHANGED) {
            fprintf(stderr, "  format changed, continuing...\n");
            stalled = 0;
        } else if (err == ERROR_END_OF_STREAM || err == -ENODATA) {
            fprintf(stderr, "  EOS after %d frames\n", framesOut);
            break;
        } else if (err != -EWOULDBLOCK) {
            fprintf(stderr, "  read returned %d (0x%x)\n", err, -err);
            if (err != OK) usleep(10000);
        } else {
            usleep(10000);
            stalled++;
            if (stalled > 500) {  // 5 seconds
                fprintf(stderr, "  stalled >5s on -EWOULDBLOCK, breaking\n");
                break;
            }
        }
    }

    // 8. Stop and cleanup. AsyncCodecSource drains any pending output
    // buffers during stop; explicitly release the codec and looper before
    // leaving main so no MediaBuffer survives process teardown.
    codec->stop();
    codec.clear();
    looper->stop();
    if (outFd >= 0) close(outFd);
    fprintf(stderr, "[6/7] encoder stopped\n");

    // 9. Report
    struct stat st;
    if (framesOut == N && stat(out, &st) == 0 && st.st_size > 0) {
        fprintf(stderr, "=== ENCODER HARNESS PASSED (%s mode) ===\n",
                metadataMode ? "zero-copy" : "RGBA-copy");
        fprintf(stderr, "    Output: %s (%ld bytes, %d frames)\n",
                out, st.st_size, framesOut);
        return 0;
    } else {
        fprintf(stderr, "=== ENCODER HARNESS FAILED: output incomplete (%d/%d frames polled) ===\n",
                        framesOut, N);
        return 1;
    }
}