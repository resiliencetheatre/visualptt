/*
 * visualptt-tx
 * 
 * Copyright (C) 2025 Resilience Theatre
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 * Ini file template:
 * 
 * [pttkey]
 * keyboard_device=/dev/input/event0
 * ptt_down_type = 1 
 * ptt_down_code = 100 
 * ptt_down_value = 1 
 * ptt_up_type = 1 
 * ptt_up_code = 100 
 * ptt_up_value = 0
 * output_dir = /path/to/spooldir      ; optional, where finalized MKVs are moved
 * indicator_file = /path/to/flagfile  ; optional, touched on PTT down, deleted on PTT up
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <linux/input.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>

#include <gst/gst.h>

#include "log.h"
#include "ini.h"

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

/* Create timestamped filename like rec_20251117_123456.mkv */
static void make_timestamp_filename(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "rec_%Y%m%d_%H%M%S.mkv", &tm);
}

/* Start GStreamer recording pipeline, return GstElement* or NULL on error */
static GstElement *start_recording_pipeline(const char *filename)
{
    char pipeline_str[1024];

    /* You can tweak devices / bitrates here if needed. */
    snprintf(pipeline_str, sizeof(pipeline_str),

        "v4l2src device=/dev/video0 ! "
        "videoconvert ! videoscale ! "
        "video/x-raw,width=160,height=120,format=I420 ! "
        "videorate ! video/x-raw,framerate=5/1 ! "
        "x264enc bitrate=100 speed-preset=veryfast "
            "key-int-max=10 tune=zerolatency byte-stream=true ! "
        "h264parse config-interval=-1 ! queue ! mux. "
        "alsasrc device=hw:0 ! audioconvert ! audioresample ! "
        "audio/x-raw,rate=48000,channels=1 ! "
        "opusenc bitrate=8000 frame-size=40 complexity=5 ! "
        "queue ! mux. "
        "matroskamux name=mux streamable=true ! "
        "filesink location=%s",

        filename
    );

    log_debug("Starting GStreamer pipeline: %s", pipeline_str);

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_str, &err);
    if (!pipeline) {
        log_error("gst_parse_launch() failed: %s", err ? err->message : "unknown error");
        if (err) g_error_free(err);
        return NULL;
    }
    if (err) {
        /* Non-fatal parse warnings */
        log_warn("GStreamer parse warning: %s", err->message);
        g_error_free(err);
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        log_error("Failed to set pipeline to PLAYING");
        gst_object_unref(pipeline);
        return NULL;
    }

    log_info("Recording started to file: %s", filename);
    return pipeline;
}

/* Stop GStreamer pipeline cleanly: send EOS (best-effort), wait a bit, then set NULL & unref */
static void stop_recording_pipeline(GstElement *pipeline)
{
    if (!pipeline)
        return;

    log_info("Stopping recording...");

    GstBus *bus = gst_element_get_bus(pipeline);
    if (!bus) {
        log_warn("No bus from pipeline, forcing NULL state");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return;
    }

    /* Try to send EOS so matroskamux can finalize file */
    if (!gst_element_send_event(pipeline, gst_event_new_eos())) {
        log_warn("Failed to send EOS event, stopping pipeline anyway");
    }

    /* Wait up to a few seconds for EOS/ERROR, don't block forever */
    const GstClockTime timeout = 3 * GST_SECOND;
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, timeout,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR
    );

    if (msg) {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            log_debug("Received EOS from pipeline");
            break;
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug_info = NULL;
            gst_message_parse_error(msg, &err, &debug_info);
            log_error("GStreamer ERROR: %s", err->message);
            if (debug_info) {
                log_debug("Debug info: %s", debug_info);
                g_free(debug_info);
            }
            if (err) g_error_free(err);
            break;
        }
        default:
            break;
        }
        gst_message_unref(msg);
    } else {
        log_warn("No EOS/ERROR within timeout, forcing pipeline to NULL");
    }

    gst_object_unref(bus);

    /* In any case, force shutdown now */
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    log_info("Recording stopped.");
}

/* Ensure output directory exists (best-effort).
 * Returns 0 if it exists or was created, -1 on error.
 */
static int ensure_output_dir(const char *path)
{
    if (!path || !*path)
        return -1;

    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0; /* already exists */
        } else {
            log_error("output_dir '%s' exists but is not a directory", path);
            return -1;
        }
    }

    /* Try to create it (mode 0755) */
    if (mkdir(path, 0755) == 0) {
        log_info("Created output_dir: %s", path);
        return 0;
    } else {
        log_error("Failed to create output_dir '%s': %s",
                  path, strerror(errno));
        return -1;
    }
}

/* Move finished file to output_dir (if configured) */
static void move_finished_file(const char *filename, const char *output_dir)
{
    if (!filename || !*filename) {
        return;
    }

    if (!output_dir || !*output_dir) {
        /* No output_dir configured, leave file where it is */
        return;
    }

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/%s", output_dir, filename);

    if (rename(filename, dest) == 0) {
        log_info("Moved '%s' -> '%s'", filename, dest);
    } else {
        log_error("Failed to move '%s' to '%s': %s",
                  filename, dest, strerror(errno));
    }
}

/* Create/touch indicator file with a timestamp */
static void create_indicator_file(const char *path)
{
    if (!path || !*path)
        return;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("Failed to create indicator_file '%s': %s",
                  path, strerror(errno));
        return;
    }

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S\n", &tm);

    ssize_t w = write(fd, buf, strlen(buf));
    (void)w; /* ignore short write errors for this simple flag */

    close(fd);
    log_info("Created indicator_file '%s'", path);
}

/* Remove indicator file if it exists */
static void remove_indicator_file(const char *path)
{
    if (!path || !*path)
        return;

    if (unlink(path) == 0) {
        log_info("Removed indicator_file '%s'", path);
    } else {
        if (errno != ENOENT) {
            log_warn("Failed to remove indicator_file '%s': %s",
                     path, strerror(errno));
        }
    }
}

int main(int argc, char *argv[])
{
    int opt;
    log_set_level(LOG_INFO);
    log_set_quiet(1);

    /* Command line options */
    while ((opt = getopt(argc, argv, "dlh")) != -1) {
        switch (opt) {
        case 'h':
            fprintf(stderr, "\npttkey_rec\n");
            fprintf(stderr, "\n Options: -l enable logging\n");
            fprintf(stderr, "          -d set log level to debug\n\n");
            return 0;
        case 'l':
            log_set_quiet(0);
            break;
        case 'd':
            log_set_level(LOG_DEBUG);
            break;
        default:
            break;
        }
    }

    /* GStreamer init */
    gst_init(&argc, &argv);

    signal(SIGINT, handle_sigint);

    int state = 0; /* 0 = idle, 1 = PTT down / recording */
    int rcode = 0;
    char keyboard_name[256] = "Unknown";
    char *keyboard_device = NULL;
    char *ptt_down_command = NULL; /* currently unused */
    char *ptt_up_command = NULL;   /* currently unused */
    char *ptt_down_type, *ptt_down_code, *ptt_down_value;
    char *ptt_up_type, *ptt_up_code, *ptt_up_value;
    char *output_dir = NULL;       /* output directory for finalized files */
    char *indicator_file = NULL;   /* indicator file path */

    /* Read ini-file */
    ini_t *config = ini_load("pttkey.ini");
    if (config == NULL) {
        log_error("[%d] Cannot find pttkey.ini, aborting.", getpid());
        exit(1);
    }

    ini_sget(config, "pttkey", "keyboard_device", NULL, &keyboard_device);
    ini_sget(config, "pttkey", "ptt_down_command", NULL, &ptt_down_command);
    ini_sget(config, "pttkey", "ptt_up_command", NULL, &ptt_up_command);
    ini_sget(config, "pttkey", "ptt_down_type", NULL, &ptt_down_type);
    ini_sget(config, "pttkey", "ptt_down_code", NULL, &ptt_down_code);
    ini_sget(config, "pttkey", "ptt_down_value", NULL, &ptt_down_value);
    ini_sget(config, "pttkey", "ptt_up_type", NULL, &ptt_up_type);
    ini_sget(config, "pttkey", "ptt_up_code", NULL, &ptt_up_code);
    ini_sget(config, "pttkey", "ptt_up_value", NULL, &ptt_up_value);
    ini_sget(config, "pttkey", "output_dir", NULL, &output_dir);
    ini_sget(config, "pttkey", "indicator_file", NULL, &indicator_file);

    if (!keyboard_device) {
        log_error("keyboard_device not set in pttkey.ini");
        ini_free(config);
        exit(1);
    }

    if (output_dir && *output_dir) {
        if (ensure_output_dir(output_dir) != 0) {
            log_warn("Continuing without output_dir move support.");
            output_dir = NULL;
        } else {
            log_info("Using output_dir: %s", output_dir);
        }
    }

    if (indicator_file && *indicator_file) {
        log_info("Using indicator_file: %s", indicator_file);
        /* Ensure no stale indicator exists on start */
        remove_indicator_file(indicator_file);
    }

    /* Open keyboard */
    int keyboard_fd = open(keyboard_device, O_RDONLY | O_NONBLOCK);
    if (keyboard_fd == -1) {
        log_error("[%d] Failed to open keyboard %s. Are you in input group?",
                  getpid(), keyboard_device);
        ini_free(config);
        exit(1);
    }

    rcode = ioctl(keyboard_fd, EVIOCGNAME(sizeof(keyboard_name)), keyboard_name);
    (void)rcode; /* not critical if it fails */

    log_debug("[%d] Monitoring %s", getpid(), keyboard_name);
    log_info("[%d] Keyboard open %s", getpid(), keyboard_device);

    struct input_event keyboard_event;

    GstElement *pipeline = NULL; /* current recording pipeline, or NULL */
    char current_filename[128] = {0}; /* holds the base name of the current recording */

    while (g_running) {

        ssize_t n = read(keyboard_fd, &keyboard_event, sizeof(keyboard_event));
        if (n == (ssize_t)sizeof(keyboard_event)) {

            /* PTT down */
            if (state == 0 &&
                keyboard_event.type  == atoi(ptt_down_type) &&
                keyboard_event.code  == atoi(ptt_down_code) &&
                keyboard_event.value == atoi(ptt_down_value))
            {
                log_info("[%d] PTT down", getpid());
                state = 1;

                make_timestamp_filename(current_filename, sizeof(current_filename));
                log_info("[%d] Starting recording to %s", getpid(), current_filename);

                pipeline = start_recording_pipeline(current_filename);
                if (!pipeline) {
                    log_error("[%d] Failed to start recording pipeline", getpid());
                    /* safer to revert to idle state */
                    state = 0;
                    current_filename[0] = '\0';
                } else {
                    /* Touch indicator file to signal "incoming message" */
                    create_indicator_file(indicator_file);
                }
            }

            /* PTT release */
            if (state == 1 &&
                keyboard_event.type  == atoi(ptt_up_type) &&
                keyboard_event.code  == atoi(ptt_up_code) &&
                keyboard_event.value == atoi(ptt_up_value))
            {
                log_info("[%d] PTT up", getpid());
                state = 0;

                if (pipeline) {
                    stop_recording_pipeline(pipeline);
                    pipeline = NULL;

                    /* Move the finalized file into output_dir (if configured) */
                    if (current_filename[0] != '\0') {
                        move_finished_file(current_filename, output_dir);
                        current_filename[0] = '\0';
                    }
                }

                /* Remove indicator file now that transmission ended */
                remove_indicator_file(indicator_file);
            }
        }

        /* Small sleep to avoid a tight busy loop; ~10 ms */
        usleep(1000 * 10);
    }

    log_info("[%d] Exiting", getpid());

    /* If we exit while still recording, stop pipeline and clean up indicator */
    if (pipeline) {
        stop_recording_pipeline(pipeline);
        pipeline = NULL;
        if (current_filename[0] != '\0') {
            move_finished_file(current_filename, output_dir);
            current_filename[0] = '\0';
        }
    }

    /* Make sure indicator file is gone on exit */
    remove_indicator_file(indicator_file);

    close(keyboard_fd);
    ini_free(config);

    return 0;
}
