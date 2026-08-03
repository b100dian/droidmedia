/*
 * Opaque screen-capture bridge exported by libminisf.
 *
 * The QPA plugin already loads libminisf at runtime. Keeping these entry
 * points here avoids a compile-time dependency from the boot-critical QPA
 * plugin on libdroidmedia and its private C++ headers.
 */
#include "droidmedia.h"

extern "C" {

void minisf_screen_capture_init(int width, int height, void **out_queue)
{
    droid_media_screen_capture_init(
        width, height, reinterpret_cast<DroidMediaBufferQueue **>(out_queue));
}

void *minisf_screen_capture_producer(void *queue)
{
    return droid_media_screen_capture_queue_producer(
        static_cast<DroidMediaBufferQueue *>(queue));
}

void minisf_screen_capture_destroy(void *queue)
{
    droid_media_screen_capture_queue_destroy(
        static_cast<DroidMediaBufferQueue *>(queue));
}

void *minisf_screen_capture_consumer_new(void)
{
    return droid_media_screen_capture_consumer_new();
}

int minisf_screen_capture_get_dimensions(int *width, int *height)
{
    return droid_media_screen_capture_get_dimensions(width, height);
}

}
