#define _GNU_SOURCE
#include "recording.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gst/gst.h>
#include <time.h>

/* Freeze only the test executable and force one reservation collision. */
time_t __wrap_time(time_t *out)
{
    time_t now = 1790147700;
    if (out) *out = now;
    return now;
}
ssize_t __real_getrandom(void *, size_t, unsigned int);
ssize_t __wrap_getrandom(void *buf, size_t size, unsigned int flags)
{
    static int calls;
    if (calls++ < 2) { memset(buf, 0, size); return (ssize_t)size; }
    return __real_getrandom(buf, size, flags);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    char root[] = "/tmp/visualptt-recording-XXXXXX";
    assert(mkdtemp(root) && chdir(root) == 0 && mkdir("out", 0700) == 0);
    char names[2][128], paths[2][4096];
    for (int i = 0; i < 2; i++) {
        int fd = recording_reserve(names[i], sizeof names[i], paths[i], sizeof paths[i]);
        assert(fd >= 0 && recording_timestamp_offset(names[i]) == 4);
        assert(strlen(names[i]) == 56);
        /* Exercise the actual sink with a reserved descriptor and known tail:
         * fdsink must not truncate or reopen it. No capture hardware required. */
        char original[64]; memset(original, 'x', sizeof original);
        assert(write(fd, original, sizeof original) == sizeof original);
        char spec[256];
        snprintf(spec, sizeof spec, "fakesrc num-buffers=1 sizetype=fixed sizemax=16 ! fdsink fd=%d", fd);
        GError *error = NULL;
        GstElement *pipeline = gst_parse_launch(spec, &error);
        assert(pipeline && !error);
        assert(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, 5*GST_SECOND, GST_MESSAGE_EOS|GST_MESSAGE_ERROR);
        assert(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);
        gst_message_unref(msg);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        char prefix[6];
        assert(pread(fd, prefix, 6, 32) == 6 && !memcmp(prefix, "xxxxxx", 6));
        struct stat st;
        assert(fstat(fd, &st) == 0 && st.st_size == 64);
        close(fd);
    }
    assert(strcmp(names[0], names[1]));
    assert(!strncmp(names[0], names[1], 19)); /* same local second */
    assert(recording_publish(paths[0], names[0], "out") == 0);
    assert(access(paths[0], F_OK) != 0);
    /* Unexpected outgoing collision: different recording must remain intact. */
    assert(recording_publish(paths[1], names[0], "out") == -1 && errno == EEXIST);
    assert(access(paths[1], F_OK) == 0);
    assert(recording_publish(paths[1], names[1], NULL) == 0); /* standalone no output_dir */
    assert(access(names[1], F_OK) == 0);
    const char *valid[] = {"rec_20260923_091500.mkv", "A_rec_20260923_091500.mkv",
        "rec_rec_20260923_091500.mkv", "desk_rec_20240229_235959_0123456789abcdef0123456789abcdef.mkv"};
    const char *invalid[] = {"rec_20260923_091500.mkv.extra", "rec_20260923_091500.wav",
        "rec_20260923_091500", "rec_20260923_091500_ab.mkv", "rec_20260229_091500.mkv",
        "../rec_20260923_091500.mkv", "a_b_rec_20260923_091500.mkv", "rec_20260923_251500.mkv",
        "rec_20260923_091500_0123456789ABCDEF0123456789ABCDEF.mkv"};
    for (size_t i=0; i < sizeof valid/sizeof *valid; i++) assert(recording_timestamp_offset(valid[i]) >= 0);
    for (size_t i=0; i < sizeof invalid/sizeof *invalid; i++) assert(recording_timestamp_offset(invalid[i]) < 0);
    char published[256];
    snprintf(published, sizeof published, "out/%s", names[0]);
    unlink(published); unlink(names[1]); rmdir("out"); chdir("/"); rmdir(root);
    puts("recording reservation, actual fdsink, publication and basename tests passed");
    return 0;
}
