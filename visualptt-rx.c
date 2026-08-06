/*
 * visualptt-rx (GTK3) - watch a directory for .mkv files, play them in a
 * single GTK window via GStreamer (gtksink), then delete after playback.
 * 
 * Copyright (C) 2025 Resilience Theatre
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 * 
 * Idle state shows "END OF TRANSMISSION" or "...incoming message"
 * depending on presence of "incoming.flag" in the watch directory.
 * 
 * Dependencies (Debian):
 *   sudo apt install \
 *      libgtk-3-dev \
 *      libgstreamer1.0-dev \
 *      gstreamer1.0-plugins-base \
 *      gstreamer1.0-plugins-good \
 *      gstreamer1.0-plugins-bad \
 *      gstreamer1.0-plugins-ugly
 *
 * Build example:
 *   gcc -Wall -O2 visualptt-rx.c -o visualptt-rx \
 *       `pkg-config --cflags --libs gtk+-3.0 gstreamer-1.0`
 */

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

typedef struct {
    GtkApplication *app;
    GtkWindow      *window;
    GtkLabel       *status_label;

    GstElement     *playbin;
    gboolean        playing;
    char            current_file[PATH_MAX];

    char            watch_dir[PATH_MAX];
    char            indicator_path[PATH_MAX]; /* watch_dir + "/incoming.flag" */

    GtkWidget      *stack;        /* GtkStack with video + idle */
    GtkWidget      *video_widget; /* widget from gtksink */
    GtkWidget      *idle_box;     /* idle screen container */
    GtkWidget      *idle_label;   /* label inside idle_box */
} AppState;

/* Return 1 if name ends with ".mkv", else 0 */
static int has_mkv_extension(const char *name)
{
    size_t len = strlen(name);
    if (len < 5) return 0; /* ".mkv" is 4 chars */
    return (strcmp(name + len - 4, ".mkv") == 0);
}

/* Find the oldest *.mkv file that looks "complete" in dirpath.
 * "Complete" here means: mtime at least 1 second in the past.
 * Returns 1 and writes the path into out_path if found, else 0.
 */
static int find_oldest_ready_mkv(const char *dirpath, char *out_path, size_t out_len)
{
    DIR *dir = opendir(dirpath);
    if (!dir) {
        g_printerr("opendir('%s') failed: %s\n", dirpath, g_strerror(errno));
        return 0;
    }

    struct dirent *de;
    time_t now = time(NULL);
    time_t oldest_mtime = 0;
    char candidate[PATH_MAX] = {0};

    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (!has_mkv_extension(de->d_name))
            continue;

        char path[PATH_MAX];
        int n = g_snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);
        if (n <= 0 || n >= (int)sizeof(path)) {
            g_printerr("Path too long, skipping: %s/%s\n", dirpath, de->d_name);
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        /* Consider it ready if it's older than 1 second */
        if ((now - st.st_mtime) < 1)
            continue;

        if (candidate[0] == '\0' || st.st_mtime < oldest_mtime) {
            oldest_mtime = st.st_mtime;
            g_strlcpy(candidate, path, sizeof(candidate));
        }
    }

    closedir(dir);

    if (candidate[0] == '\0')
        return 0;

    g_strlcpy(out_path, candidate, out_len);
    return 1;
}

/* Update idle label text based on indicator file presence */
static void update_idle_label(AppState *state)
{
    if (!state || !state->idle_label)
        return;

    if (state->indicator_path[0] != '\0' &&
        access(state->indicator_path, F_OK) == 0)
    {
        /* incoming.flag exists */
        gtk_label_set_markup(GTK_LABEL(state->idle_label),
                             "<span size=\"xx-large\" weight=\"bold\">...incoming message</span>");
    } else {
        /* no flag: default idle text */
        gtk_label_set_markup(GTK_LABEL(state->idle_label),
                             "<span size=\"xx-large\" weight=\"bold\">END OF TRANSMISSION</span>");
    }
}

/* Bus callback: handle EOS / ERROR for playbin */
static gboolean bus_message_cb(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    (void)bus;
    AppState *state = (AppState *)user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        g_print("EOS reached for %s\n", state->current_file);

        /* Stop playback & reset pipeline to READY so we can reuse it */
        gst_element_set_state(state->playbin, GST_STATE_READY);
        state->playing = FALSE;

        /* Delete file after successful playback */
        if (state->current_file[0] != '\0') {
            g_print("Deleting played file: %s\n", state->current_file);
            if (unlink(state->current_file) != 0) {
                g_printerr("Failed to delete %s: %s\n",
                           state->current_file, g_strerror(errno));
            }
            state->current_file[0] = '\0';
        }

        if (state->status_label) {
            gtk_label_set_text(state->status_label, "Idle");
        }

        /* Show idle screen & update its text */
        if (state->stack) {
            update_idle_label(state);
            gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");
        }

        break;

    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug_info = NULL;
        gst_message_parse_error(msg, &err, &debug_info);
        g_printerr("GStreamer ERROR: %s\n",
                   err ? err->message : "unknown error");
        if (debug_info) {
            g_printerr("Debug info: %s\n", debug_info);
            g_free(debug_info);
        }
        if (err) g_error_free(err);

        /* Stop pipeline */
        gst_element_set_state(state->playbin, GST_STATE_READY);
        state->playing = FALSE;

        /* Drop bad file so we don't loop on it */
        if (state->current_file[0] != '\0') {
            g_printerr("Deleting problematic file: %s\n", state->current_file);
            if (unlink(state->current_file) != 0) {
                g_printerr("Failed to delete %s: %s\n",
                           state->current_file, g_strerror(errno));
            }
            state->current_file[0] = '\0';
        }

        if (state->status_label) {
            gtk_label_set_text(state->status_label,
                               "Error during playback (file dropped)");
        }

        /* Back to idle screen, with updated text */
        if (state->stack) {
            update_idle_label(state);
            gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");
        }

        break;
    }

    default:
        break;
    }

    return TRUE; /* keep watching bus */
}

/* Start playing one MKV file */
static void start_playback(AppState *state, const char *filename)
{
    if (state->playing) {
        return;
    }

    g_strlcpy(state->current_file, filename, sizeof(state->current_file));
    g_print("Starting playback of %s\n", state->current_file);

    if (state->status_label) {
        char buf[256];
        /* Limit filename length in UI to avoid truncation warnings */
        snprintf(buf, sizeof(buf), "Playing: %.200s", state->current_file);
        gtk_label_set_text(state->status_label, buf);
    }

    GError *err = NULL;
    gchar *uri = gst_filename_to_uri(filename, &err);
    if (!uri) {
        g_printerr("gst_filename_to_uri() failed for %s: %s\n",
                   filename, err ? err->message : "unknown error");
        if (err) g_error_free(err);
        state->current_file[0] = '\0';
        return;
    }

    g_object_set(state->playbin, "uri", uri, NULL);
    g_free(uri);

    GstStateChangeReturn sret =
        gst_element_set_state(state->playbin, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to set playbin to PLAYING\n");
        state->current_file[0] = '\0';
        if (state->status_label) {
            gtk_label_set_text(state->status_label,
                               "Failed to start playback");
        }

        /* Show idle screen again */
        if (state->stack) {
            update_idle_label(state);
            gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");
        }
        return;
    }

    state->playing = TRUE;

    /* Switch to video view */
    if (state->stack) {
        gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "video");
    }
}

/* Periodic timer callback: check directory for new MKV files and indicator file */
static gboolean poll_directory_cb(gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    if (!state || !state->watch_dir[0])
        return G_SOURCE_CONTINUE;

    /* If idle, update idle text based on indicator file presence */
    if (!state->playing) {
        update_idle_label(state);
    }

    /* If already playing, do nothing about files; EOS/ERROR will clear 'playing'. */
    if (state->playing)
        return G_SOURCE_CONTINUE;

    char path[PATH_MAX];
    if (find_oldest_ready_mkv(state->watch_dir, path, sizeof(path))) {
        start_playback(state, path);
    }

    return G_SOURCE_CONTINUE; /* keep timer */
}

/* Handle window close: stop GStreamer and quit application */
static gboolean on_delete_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void)widget;
    (void)event;
    AppState *state = (AppState *)user_data;

    if (state->playbin) {
        gst_element_set_state(state->playbin, GST_STATE_NULL);
        gst_object_unref(state->playbin);
        state->playbin = NULL;
    }

    g_application_quit(G_APPLICATION(state->app));
    return TRUE; /* we handled it */
}

/* GtkApplication "activate" handler: build UI and initialize GStreamer elements */
static void on_app_activate(GtkApplication *app, gpointer user_data)
{
    AppState *state = (AppState *)user_data;
    state->app = app;

    /* Create main window */
    state->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(state->window, "Visual PTT");
    gtk_window_set_default_size(state->window, 320, 240);

    /* Vertical box: stack (video/idle) on top, status label below */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(state->window), vbox);

    /* Stack containing video widget + idle screen */
    state->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(state->stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(state->stack), 200);
    gtk_widget_set_hexpand(state->stack, TRUE);
    gtk_widget_set_vexpand(state->stack, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), state->stack, TRUE, TRUE, 0);

    /* --- GStreamer: playbin + gtksink --- */
    state->playbin = gst_element_factory_make("playbin", "player");
    if (!state->playbin) {
        g_printerr("Failed to create playbin. Check GStreamer installation.\n");
        GtkWidget *lbl = gtk_label_new("Error: playbin not available");
        gtk_box_pack_start(GTK_BOX(vbox), lbl, TRUE, TRUE, 0);
        gtk_widget_show_all(GTK_WIDGET(state->window));
        return;
    }

    GstElement *video_sink = gst_element_factory_make("gtksink", "videosink");
    if (!video_sink) {
        g_printerr("Failed to create gtksink. Is gstreamer1.0-plugins-bad installed?\n");
        GtkWidget *lbl = gtk_label_new("Error: gtksink not available");
        gtk_box_pack_start(GTK_BOX(vbox), lbl, TRUE, TRUE, 0);
        gtk_widget_show_all(GTK_WIDGET(state->window));
        return;
    }

    /* Get GTK widget from gtksink and add to stack as "video" */
    GtkWidget *video_widget = NULL;
    g_object_get(video_sink, "widget", &video_widget, NULL);
    if (!video_widget) {
        g_printerr("gtksink did not provide a widget.\n");
        GtkWidget *lbl = gtk_label_new("Error: gtksink widget missing");
        gtk_box_pack_start(GTK_BOX(vbox), lbl, TRUE, TRUE, 0);
        gtk_widget_show_all(GTK_WIDGET(state->window));
        return;
    }

    state->video_widget = video_widget;
    gtk_widget_set_hexpand(video_widget, TRUE);
    gtk_widget_set_vexpand(video_widget, TRUE);

    gtk_stack_add_named(GTK_STACK(state->stack),
                        state->video_widget,
                        "video");

    /* Idle screen: label centered with markup */
    state->idle_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    state->idle_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(state->idle_label),
                         "<span size=\"xx-large\" weight=\"bold\">END OF TRANSMISSION</span>");

    gtk_box_pack_start(GTK_BOX(state->idle_box),
                       state->idle_label,
                       TRUE, TRUE, 0);

    gtk_widget_set_hexpand(state->idle_box, TRUE);
    gtk_widget_set_vexpand(state->idle_box, TRUE);

    gtk_widget_set_halign(state->idle_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->idle_label, GTK_ALIGN_CENTER);

    gtk_stack_add_named(GTK_STACK(state->stack),
                        state->idle_box,
                        "idle");

    /* Start with idle visible */
    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");

    /* Tell playbin to use gtksink for video; playbin keeps its own ref */
    g_object_set(state->playbin, "video-sink", video_sink, NULL);
    gst_object_unref(video_sink);

    /* Status label */
    state->status_label =
        GTK_LABEL(gtk_label_new("Idle"));
    gtk_box_pack_start(GTK_BOX(vbox),
                       GTK_WIDGET(state->status_label),
                       FALSE, FALSE, 2);

    /* Hook up GStreamer bus to our callback (runs in GLib main loop) */
    GstBus *bus = gst_element_get_bus(state->playbin);
    gst_bus_add_watch(bus, bus_message_cb, state);
    gst_object_unref(bus);

    /* Poll directory every 200 ms */
    g_timeout_add(200, poll_directory_cb, state);

    /* Close handler */
    g_signal_connect(state->window, "delete-event",
                     G_CALLBACK(on_delete_event), state);

    gtk_widget_show_all(GTK_WIDGET(state->window));
}

int main(int argc, char *argv[])
{
    AppState state;
    memset(&state, 0, sizeof(state));

    /* Directory to watch; default '.' */
    const char *dirpath = ".";
    if (argc >= 2) {
        dirpath = argv[1];
    }
    g_strlcpy(state.watch_dir, dirpath, sizeof(state.watch_dir));
    g_print("visualptt-rx (GTK3): watching directory %s for *.mkv files\n",
            state.watch_dir);

    /* Indicator file path: watch_dir/incoming.flag */
    int n = g_snprintf(state.indicator_path,
                       sizeof(state.indicator_path),
                       "%s/%s",
                       state.watch_dir,
                       "incoming.flag");
    if (n <= 0 || n >= (int)sizeof(state.indicator_path)) {
        g_printerr("indicator_path too long, disabling indicator: %s/%s\n",
                   state.watch_dir, "incoming.flag");
        state.indicator_path[0] = '\0';
    }

    /* Initialize GStreamer (before GtkApplication run) */
    gst_init(&argc, &argv);

    GtkApplication *app = gtk_application_new("org.resiliencetheatre.visualptt_rx",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), &state);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}
