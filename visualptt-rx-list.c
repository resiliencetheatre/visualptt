/*
 * visualptt-rx-list (GTK3) - persistent visual message receiver.
 * Copyright (C) 2025 Resilience Theatre
 *
 * Watches a directory for completed .mkv files, displays them by timestamp,
 * and plays a message when its row is selected.  Auto play is enabled by
 * default.  Unlike visualptt-rx, received files are never removed.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    COL_TIMESTAMP,
    COL_PATH,
    COL_MTIME,
    N_COLUMNS
};

typedef struct {
    GtkApplication *app;
    GtkWindow *window;
    GtkWidget *stack;
    GtkWidget *idle_label;
    GtkLabel *status_label;
    GtkTextBuffer *annotation_buffer;
    GtkToggleButton *auto_play;
    GtkListStore *messages;
    GtkTreeSelection *selection;
    GHashTable *known_files;
    GQueue *auto_queue;
    GstDiscoverer *discoverer;
    GstElement *playbin;
    gboolean playing;
    gboolean selecting_programmatically;
    char current_file[PATH_MAX];
    char annotation_media_file[PATH_MAX];
    time_t annotation_mtime;
    off_t annotation_size;
    gboolean annotation_present;
    guint annotation_clear_delay_seconds;
    guint annotation_clear_source;
    char watch_dir[PATH_MAX];
    char indicator_path[PATH_MAX];
} AppState;

#define DEFAULT_ANNOTATION_CLEAR_DELAY_SECONDS 30

static gboolean has_mkv_extension(const char *name)
{
    size_t len = strlen(name);
    return len >= 4 && strcmp(name + len - 4, ".mkv") == 0;
}

static void refresh_annotation(AppState *state, const char *media_file,
                               gboolean force)
{
    char text_file[PATH_MAX];
    struct stat info;
    gchar *contents = NULL;
    gchar *display_text = NULL;
    gsize length = 0;
    size_t media_length;

    if (!state->annotation_buffer || !media_file)
        return;

    media_length = strlen(media_file);
    if (media_length < 4 ||
        g_snprintf(text_file, sizeof(text_file), "%.*s.txt",
                   (int)(media_length - 4), media_file) >= (int)sizeof(text_file))
        return;

    if (stat(text_file, &info) != 0 || !S_ISREG(info.st_mode)) {
        if (force || state->annotation_present ||
            g_strcmp0(state->annotation_media_file, media_file) != 0)
            gtk_text_buffer_set_text(state->annotation_buffer, "", -1);
        g_strlcpy(state->annotation_media_file, media_file,
                  sizeof(state->annotation_media_file));
        state->annotation_present = FALSE;
        state->annotation_mtime = 0;
        state->annotation_size = 0;
        return;
    }

    if (!force && state->annotation_present &&
        g_strcmp0(state->annotation_media_file, media_file) == 0 &&
        state->annotation_mtime == info.st_mtime &&
        state->annotation_size == info.st_size)
        return;

    if (g_file_get_contents(text_file, &contents, &length, NULL)) {
        display_text = g_utf8_make_valid(contents, length);
        gtk_text_buffer_set_text(state->annotation_buffer, display_text, -1);
        g_free(display_text);
        g_free(contents);
        state->annotation_present = TRUE;
        state->annotation_mtime = info.st_mtime;
        state->annotation_size = info.st_size;
    } else {
        gtk_text_buffer_set_text(state->annotation_buffer, "", -1);
        state->annotation_present = FALSE;
        state->annotation_mtime = 0;
        state->annotation_size = 0;
    }
    g_strlcpy(state->annotation_media_file, media_file,
              sizeof(state->annotation_media_file));
}

static void cancel_annotation_clear(AppState *state)
{
    if (state->annotation_clear_source != 0) {
        g_source_remove(state->annotation_clear_source);
        state->annotation_clear_source = 0;
    }
}

static void clear_annotation(AppState *state)
{
    cancel_annotation_clear(state);
    if (state->annotation_buffer)
        gtk_text_buffer_set_text(state->annotation_buffer, "", -1);
    state->annotation_media_file[0] = '\0';
    state->annotation_present = FALSE;
    state->annotation_mtime = 0;
    state->annotation_size = 0;
}

static gboolean clear_annotation_cb(gpointer data)
{
    AppState *state = data;

    state->annotation_clear_source = 0;
    clear_annotation(state);
    return G_SOURCE_REMOVE;
}

static void schedule_annotation_clear(AppState *state)
{
    cancel_annotation_clear(state);
    if (!state->annotation_present)
        return;
    if (state->annotation_clear_delay_seconds == 0) {
        clear_annotation(state);
        return;
    }
    state->annotation_clear_source = g_timeout_add_seconds(
        state->annotation_clear_delay_seconds, clear_annotation_cb, state);
}

static void update_idle_label(AppState *state)
{
    const char *markup =
        access(state->indicator_path, F_OK) == 0
        ? "<span size=\"xx-large\" weight=\"bold\">...incoming message</span>"
        : "<span size=\"xx-large\" weight=\"bold\">END OF TRANSMISSION</span>";
    gtk_label_set_markup(GTK_LABEL(state->idle_label), markup);
}

static void stop_playback(AppState *state)
{
    gst_element_set_state(state->playbin, GST_STATE_READY);
    state->playing = FALSE;
    state->current_file[0] = '\0';
}

static void start_playback(AppState *state, const char *filename)
{
    GError *error = NULL;
    gchar *uri;

    if (!filename || !*filename || access(filename, R_OK) != 0) {
        gtk_label_set_text(state->status_label, "Message file is no longer available");
        return;
    }

    cancel_annotation_clear(state);
    refresh_annotation(state, filename, TRUE);

    if (state->playing)
        stop_playback(state);

    uri = gst_filename_to_uri(filename, &error);
    if (!uri) {
        g_printerr("Cannot create URI for %s: %s\n", filename,
                   error ? error->message : "unknown error");
        g_clear_error(&error);
        gtk_label_set_text(state->status_label, "Could not open message");
        return;
    }

    g_strlcpy(state->current_file, filename, sizeof(state->current_file));
    g_object_set(state->playbin, "uri", uri, NULL);
    g_free(uri);

    if (gst_element_set_state(state->playbin, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        state->current_file[0] = '\0';
        gtk_label_set_text(state->status_label, "Failed to start playback");
        return;
    }

    state->playing = TRUE;
    gtk_label_set_text(state->status_label, "Playing selected message");
    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "video");
}

static void play_next_queued(AppState *state)
{
    char *filename;

    if (state->playing || g_queue_is_empty(state->auto_queue))
        return;
    filename = g_queue_pop_head(state->auto_queue);
    start_playback(state, filename);
    g_free(filename);
}

static gboolean bus_message_cb(GstBus *bus, GstMessage *message, gpointer data)
{
    AppState *state = data;
    (void)bus;

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        stop_playback(state);
        schedule_annotation_clear(state);
        gtk_label_set_text(state->status_label, "Idle");
        update_idle_label(state);
        gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");
        play_next_queued(state);
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        g_printerr("Playback error for %s: %s\n", state->current_file,
                   error ? error->message : "unknown error");
        g_clear_error(&error);
        g_free(debug);
        stop_playback(state);
        clear_annotation(state);
        gtk_label_set_text(state->status_label, "Playback error (file retained)");
        update_idle_label(state);
        gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");
        play_next_queued(state);
    }
    return G_SOURCE_CONTINUE;
}

static void select_and_maybe_play(AppState *state, GtkTreeIter *iter,
                                  gboolean auto_play)
{
    GtkTreePath *tree_path;
    gchar *filename = NULL;

    state->selecting_programmatically = TRUE;
    tree_path = gtk_tree_model_get_path(GTK_TREE_MODEL(state->messages), iter);
    gtk_tree_selection_select_path(state->selection, tree_path);
    gtk_tree_path_free(tree_path);
    state->selecting_programmatically = FALSE;

    if (auto_play) {
        gtk_tree_model_get(GTK_TREE_MODEL(state->messages), iter,
                           COL_PATH, &filename, -1);
        start_playback(state, filename);
        g_free(filename);
    }
}

static void format_message_line(AppState *state, const char *path,
                                const struct stat *info, char *line,
                                size_t line_size)
{
    struct tm local_time;
    char timestamp[64];
    guint64 duration_seconds = 0;
    guint64 size_value;
    const char *size_unit;
    gchar *uri;
    GstDiscovererInfo *discovery = NULL;
    GError *error = NULL;

    localtime_r(&info->st_mtime, &local_time);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);

    uri = gst_filename_to_uri(path, NULL);
    if (uri && state->discoverer) {
        discovery = gst_discoverer_discover_uri(state->discoverer, uri, &error);
        if (discovery && gst_discoverer_info_get_result(discovery) ==
                         GST_DISCOVERER_OK) {
            GstClockTime duration = gst_discoverer_info_get_duration(discovery);
            if (GST_CLOCK_TIME_IS_VALID(duration))
                duration_seconds = duration / GST_SECOND;
        }
    }
    if (error) {
        g_printerr("Could not read duration for %s: %s\n", path, error->message);
        g_error_free(error);
    }
    if (discovery)
        gst_discoverer_info_unref(discovery);
    g_free(uri);

    if ((guint64)info->st_size < 1024 * 1024) {
        size_value = ((guint64)info->st_size + 512) / 1024;
        size_unit = "KB";
    } else {
        size_value = ((guint64)info->st_size + 512 * 1024) / (1024 * 1024);
        size_unit = "MB";
    }
    if (size_value > 9999)
        size_value = 9999;

    g_snprintf(line, line_size, "%s    %02" G_GUINT64_FORMAT
               ":%02" G_GUINT64_FORMAT " (%" G_GUINT64_FORMAT " %s)",
               timestamp, duration_seconds / 60, duration_seconds % 60,
               size_value, size_unit);
}

static gboolean add_file(AppState *state, const char *path,
                         const struct stat *info, gboolean is_new)
{
    GtkTreeIter iter;
    char line[128];

    if (g_hash_table_contains(state->known_files, path))
        return FALSE;

    format_message_line(state, path, info, line, sizeof(line));
    gtk_list_store_append(state->messages, &iter);
    gtk_list_store_set(state->messages, &iter,
                       COL_TIMESTAMP, line,
                       COL_PATH, path,
                       COL_MTIME, (gint64)info->st_mtime,
                       -1);
    g_hash_table_add(state->known_files, g_strdup(path));

    if (is_new && gtk_toggle_button_get_active(state->auto_play)) {
        g_queue_push_tail(state->auto_queue, g_strdup(path));
        if (!state->playing) {
            select_and_maybe_play(state, &iter, FALSE);
            play_next_queued(state);
        }
    }
    return TRUE;
}

static gboolean scan_directory(AppState *state, gboolean initial_scan)
{
    DIR *dir = opendir(state->watch_dir);
    struct dirent *entry;
    time_t now = time(NULL);

    if (!dir) {
        g_printerr("Cannot open %s: %s\n", state->watch_dir, g_strerror(errno));
        return G_SOURCE_CONTINUE;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct stat info;
        int length;

        if (entry->d_name[0] == '.' || !has_mkv_extension(entry->d_name))
            continue;
        length = g_snprintf(path, sizeof(path), "%s/%s",
                            state->watch_dir, entry->d_name);
        if (length <= 0 || length >= (int)sizeof(path) ||
            stat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
            now - info.st_mtime < 1)
            continue;
        add_file(state, path, &info, !initial_scan);
    }
    closedir(dir);
    return G_SOURCE_CONTINUE;
}

static void reload_messages(AppState *state)
{
    g_queue_clear_full(state->auto_queue, g_free);
    g_hash_table_remove_all(state->known_files);
    gtk_list_store_clear(state->messages);
    clear_annotation(state);
    scan_directory(state, TRUE);
}

static guint delete_annotation_files(const char *media_file)
{
    static const char *extensions[] = { ".txt", ".wav" };
    char companion[PATH_MAX];
    size_t media_length = strlen(media_file);
    guint failed = 0;
    guint i;

    if (media_length < 4)
        return 0;

    for (i = 0; i < G_N_ELEMENTS(extensions); i++) {
        int length = g_snprintf(companion, sizeof(companion), "%.*s%s",
                                (int)(media_length - 4), media_file,
                                extensions[i]);
        if (length <= 0 || length >= (int)sizeof(companion)) {
            failed++;
            continue;
        }
        if (unlink(companion) != 0 && errno != ENOENT)
            failed++;
    }
    return failed;
}

static void delete_selected_cb(GtkButton *button, gpointer data)
{
    AppState *state = data;
    GtkTreeModel *model;
    GtkTreeIter iter;
    gchar *filename = NULL;
    guint companion_failures;
    int media_delete_result;
    int media_delete_error = 0;
    (void)button;

    if (!gtk_tree_selection_get_selected(state->selection, &model, &iter)) {
        gtk_label_set_text(state->status_label, "Select a message to delete");
        return;
    }

    gtk_tree_model_get(model, &iter, COL_PATH, &filename, -1);
    if (state->playing && g_strcmp0(filename, state->current_file) == 0)
        stop_playback(state);

    companion_failures = delete_annotation_files(filename);
    media_delete_result = unlink(filename);
    if (media_delete_result != 0)
        media_delete_error = errno;
    if (media_delete_result == 0 && companion_failures == 0) {
        gtk_label_set_text(state->status_label, "Message deleted");
    } else if (media_delete_result == 0) {
        gchar *status = g_strdup_printf(
            "Message deleted; failed to delete %u annotation file%s",
            companion_failures, companion_failures == 1 ? "" : "s");
        gtk_label_set_text(state->status_label, status);
        g_free(status);
    } else {
        gchar *status = g_strdup_printf("Could not delete message: %s",
                                        g_strerror(media_delete_error));
        gtk_label_set_text(state->status_label, status);
        g_free(status);
    }
    g_free(filename);
    reload_messages(state);

    if (!state->playing) {
        update_idle_label(state);
        gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");
    }
}

static void delete_all_cb(GtkButton *button, gpointer data)
{
    AppState *state = data;
    DIR *dir;
    struct dirent *entry;
    guint deleted = 0;
    guint failed = 0;
    (void)button;

    if (state->playing)
        stop_playback(state);
    g_queue_clear_full(state->auto_queue, g_free);

    dir = opendir(state->watch_dir);
    if (!dir) {
        gchar *status = g_strdup_printf("Could not open message directory: %s",
                                        g_strerror(errno));
        gtk_label_set_text(state->status_label, status);
        g_free(status);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct stat info;
        int length;

        if (entry->d_name[0] == '.' || !has_mkv_extension(entry->d_name))
            continue;
        length = g_snprintf(path, sizeof(path), "%s/%s",
                            state->watch_dir, entry->d_name);
        if (length <= 0 || length >= (int)sizeof(path) ||
            lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            failed++;
            continue;
        }
        failed += delete_annotation_files(path);
        if (unlink(path) == 0)
            deleted++;
        else
            failed++;
    }
    closedir(dir);
    reload_messages(state);
    update_idle_label(state);
    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");

    if (failed == 0) {
        gchar *status = g_strdup_printf("Deleted %u message%s", deleted,
                                        deleted == 1 ? "" : "s");
        gtk_label_set_text(state->status_label, status);
        g_free(status);
    } else {
        gchar *status = g_strdup_printf("Deleted %u; failed to delete %u",
                                        deleted, failed);
        gtk_label_set_text(state->status_label, status);
        g_free(status);
    }
}

static gboolean poll_directory_cb(gpointer data)
{
    AppState *state = data;

    if (!state->playing)
        update_idle_label(state);
    if (state->playing)
        refresh_annotation(state, state->current_file, FALSE);
    return scan_directory(state, FALSE);
}

static void selection_changed_cb(GtkTreeSelection *selection, gpointer data)
{
    AppState *state = data;
    GtkTreeModel *model;
    GtkTreeIter iter;
    gchar *filename = NULL;

    if (state->selecting_programmatically)
        return;
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_tree_model_get(model, &iter, COL_PATH, &filename, -1);
        start_playback(state, filename);
        g_free(filename);
    }
}

static gboolean message_button_press_cb(GtkWidget *widget, GdkEventButton *event,
                                        gpointer data)
{
    AppState *state = data;
    GtkTreeView *tree = GTK_TREE_VIEW(widget);
    GtkTreeModel *model;
    GtkTreeIter selected_iter;
    GtkTreePath *clicked_path = NULL;
    GtkTreePath *selected_path;
    gchar *filename = NULL;

    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY)
        return FALSE;
    if (!gtk_tree_view_get_path_at_pos(tree, (gint)event->x, (gint)event->y,
                                       &clicked_path, NULL, NULL, NULL))
        return FALSE;
    if (!gtk_tree_selection_get_selected(state->selection, &model,
                                         &selected_iter)) {
        gtk_tree_path_free(clicked_path);
        return FALSE;
    }

    selected_path = gtk_tree_model_get_path(model, &selected_iter);
    if (gtk_tree_path_compare(clicked_path, selected_path) == 0) {
        gtk_tree_model_get(model, &selected_iter, COL_PATH, &filename, -1);
        start_playback(state, filename);
        g_free(filename);
    }
    gtk_tree_path_free(selected_path);
    gtk_tree_path_free(clicked_path);
    return FALSE;
}

static gint sort_by_mtime(GtkTreeModel *model, GtkTreeIter *a,
                         GtkTreeIter *b, gpointer data)
{
    gint64 first, second;
    (void)data;
    gtk_tree_model_get(model, a, COL_MTIME, &first, -1);
    gtk_tree_model_get(model, b, COL_MTIME, &second, -1);
    return first < second ? -1 : first > second ? 1 : 0;
}

static gboolean on_delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    AppState *state = data;
    (void)widget;
    (void)event;
    cancel_annotation_clear(state);
    if (state->playbin) {
        gst_element_set_state(state->playbin, GST_STATE_NULL);
        gst_object_unref(state->playbin);
        state->playbin = NULL;
    }
    g_application_quit(G_APPLICATION(state->app));
    return TRUE;
}

static void on_app_activate(GtkApplication *app, gpointer data)
{
    AppState *state = data;
    GtkWidget *vbox, *video_widget, *idle_box, *scroller, *tree, *checkbox;
    GtkWidget *annotation_scroller, *annotation_view;
    GtkWidget *button_box, *delete_button, *delete_all_button;
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GstElement *video_sink;
    GstBus *bus;

    state->app = app;
    state->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(state->window, "Visual PTT Messages");
    gtk_window_set_default_size(state->window, 320, 600);
    gtk_window_set_geometry_hints(state->window, NULL,
        &(GdkGeometry){ .max_width = 320, .max_height = 720 },
        GDK_HINT_MAX_SIZE);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(state->window), vbox);

    state->stack = gtk_stack_new();
    gtk_widget_set_size_request(state->stack, 320, 240);
    gtk_widget_set_hexpand(state->stack, TRUE);
    gtk_widget_set_vexpand(state->stack, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), state->stack, FALSE, FALSE, 0);

    state->playbin = gst_element_factory_make("playbin", "player");
    video_sink = gst_element_factory_make("gtksink", "videosink");
    if (!state->playbin || !video_sink) {
        g_printerr("GStreamer playbin or gtksink is unavailable\n");
        gtk_label_set_text(GTK_LABEL(gtk_label_new(NULL)), "GStreamer unavailable");
        gtk_widget_show_all(GTK_WIDGET(state->window));
        return;
    }
    video_widget = NULL;
    g_object_get(video_sink, "widget", &video_widget, NULL);
    gtk_stack_add_named(GTK_STACK(state->stack), video_widget, "video");

    idle_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    state->idle_label = gtk_label_new(NULL);
    gtk_widget_set_halign(state->idle_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->idle_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(idle_box), state->idle_label, TRUE, TRUE, 0);
    gtk_stack_add_named(GTK_STACK(state->stack), idle_box, "idle");
    update_idle_label(state);
    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "idle");

    g_object_set(state->playbin, "video-sink", video_sink, NULL);
    gst_object_unref(video_sink);

    annotation_view = gtk_text_view_new();
    state->annotation_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(annotation_view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(annotation_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(annotation_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(annotation_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(annotation_view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(annotation_view), 4);
    annotation_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(annotation_scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(annotation_scroller, -1, 76);
    gtk_container_add(GTK_CONTAINER(annotation_scroller), annotation_view);
    gtk_box_pack_start(GTK_BOX(vbox), annotation_scroller, FALSE, FALSE, 0);

    checkbox = gtk_check_button_new_with_label("Auto play");
    state->auto_play = GTK_TOGGLE_BUTTON(checkbox);
    gtk_toggle_button_set_active(state->auto_play, TRUE);

    state->messages = gtk_list_store_new(N_COLUMNS, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_INT64);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(state->messages), COL_MTIME,
                                    sort_by_mtime, NULL, NULL);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(state->messages),
                                         COL_MTIME, GTK_SORT_DESCENDING);
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(state->messages));
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(
                                                       "Received messages — length (size)",
                                                       renderer, "text",
                                                       COL_TIMESTAMP, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    state->selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
    gtk_tree_selection_set_mode(state->selection, GTK_SELECTION_SINGLE);
    g_signal_connect(state->selection, "changed",
                     G_CALLBACK(selection_changed_cb), state);
    g_signal_connect(tree, "button-press-event",
                     G_CALLBACK(message_button_press_cb), state);

    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroller, TRUE, TRUE, 0);

    button_box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(button_box), GTK_BUTTONBOX_END);
    delete_button = gtk_button_new_with_label("Delete");
    delete_all_button = gtk_button_new_with_label("Delete all");
    gtk_container_add(GTK_CONTAINER(button_box), checkbox);
    gtk_container_add(GTK_CONTAINER(button_box), delete_button);
    gtk_container_add(GTK_CONTAINER(button_box), delete_all_button);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 2);
    g_signal_connect(delete_button, "clicked",
                     G_CALLBACK(delete_selected_cb), state);
    g_signal_connect(delete_all_button, "clicked",
                     G_CALLBACK(delete_all_cb), state);

    state->status_label = GTK_LABEL(gtk_label_new("Idle"));
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(state->status_label),
                       FALSE, FALSE, 2);

    bus = gst_element_get_bus(state->playbin);
    gst_bus_add_watch(bus, bus_message_cb, state);
    gst_object_unref(bus);
    scan_directory(state, TRUE);
    g_timeout_add(200, poll_directory_cb, state);
    g_signal_connect(state->window, "delete-event",
                     G_CALLBACK(on_delete_event), state);
    gtk_widget_show_all(GTK_WIDGET(state->window));
}

int main(int argc, char **argv)
{
    AppState state = {0};
    const char *directory = argc >= 2 ? argv[1] : ".";
    int length, status;

    state.annotation_clear_delay_seconds =
        DEFAULT_ANNOTATION_CLEAR_DELAY_SECONDS;
    g_strlcpy(state.watch_dir, directory, sizeof(state.watch_dir));
    length = g_snprintf(state.indicator_path, sizeof(state.indicator_path),
                        "%s/incoming.flag", state.watch_dir);
    if (length <= 0 || length >= (int)sizeof(state.indicator_path))
        state.indicator_path[0] = '\0';
    state.known_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    state.auto_queue = g_queue_new();

    gst_init(&argc, &argv);
    GError *discoverer_error = NULL;
    state.discoverer = gst_discoverer_new(5 * GST_SECOND, &discoverer_error);
    if (!state.discoverer) {
        g_printerr("Could not create media discoverer: %s\n",
                   discoverer_error ? discoverer_error->message : "unknown error");
        g_clear_error(&discoverer_error);
    }
    GtkApplication *app = gtk_application_new(
        "org.resiliencetheatre.visualptt_rx_list", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), &state);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    if (state.discoverer)
        g_object_unref(state.discoverer);
    g_queue_free_full(state.auto_queue, g_free);
    g_hash_table_unref(state.known_files);
    if (state.messages)
        g_object_unref(state.messages);
    g_object_unref(app);
    return status;
}
