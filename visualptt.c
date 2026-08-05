/*
 * visualptt - combined GTK receiver and push-to-talk transmitter.
 *
 * The existing programs are included as implementation modules so their
 * proven playback and recording helpers can be reused without changing the
 * standalone visualptt-rx-list and visualptt-tx programs.
 */

#define main visualptt_rx_list_standalone_main
#include "visualptt-rx-list.c"
#undef main

#define main visualptt_tx_standalone_main
#include "visualptt-tx.c"
#undef main

#include <glib-unix.h>

typedef struct {
    AppState rx;
    ini_t *config;
    int keyboard_fd;
    int down_type;
    int down_code;
    int down_value;
    int up_type;
    int up_code;
    int up_value;
    int threshold_ms;
    bool pressed;
    bool recording;
    struct timespec down_at;
    GstElement *pipeline;
    char output_dir[PATH_MAX];
    char indicator_file[PATH_MAX];
    char current_filename[128];
    char current_recording_path[PATH_MAX];
    char *start_wav;
    char *end_wav;
} VisualPttState;

static int config_int(ini_t *config, const char *key, int default_value)
{
    const char *value = ini_get(config, "pttkey", key);
    return value ? atoi(value) : default_value;
}

static void set_combined_status(VisualPttState *state, const char *text)
{
    if (state->rx.status_label)
        gtk_label_set_text(state->rx.status_label, text);
}

static void finish_transmission(VisualPttState *state)
{
    if (state->pipeline) {
        stop_recording_pipeline(state->pipeline);
        state->pipeline = NULL;
    }

    if (state->current_filename[0] && state->current_recording_path[0]) {
        move_finished_file(state->current_recording_path,
                           state->current_filename, state->output_dir);
    }
    state->current_filename[0] = '\0';
    state->current_recording_path[0] = '\0';
    state->recording = false;
    remove_indicator_file(state->indicator_file);
    set_combined_status(state, "Idle");

    /* Kept consistent with visualptt-tx: the end sound is configured but
     * currently disabled there as well. */
    (void)state->end_wav;
}

static void begin_transmission(VisualPttState *state)
{
    make_timestamp_filename(state->current_filename,
                            sizeof(state->current_filename));
    if (make_absolute_path(state->current_filename,
                           state->current_recording_path,
                           sizeof(state->current_recording_path)) != 0) {
        state->pressed = false;
        return;
    }

    state->pipeline = start_recording_pipeline(state->current_recording_path);
    if (!state->pipeline) {
        state->current_filename[0] = '\0';
        state->current_recording_path[0] = '\0';
        state->pressed = false;
        set_combined_status(state, "Transmitter failed to start");
        return;
    }

    state->recording = true;
    create_indicator_file(state->indicator_file);
    set_combined_status(state, "Transmitting");
    play_wav(state->start_wav);
}

static gboolean transmitter_poll_cb(gpointer data)
{
    VisualPttState *state = data;
    struct input_event event;
    ssize_t bytes;

    while ((bytes = read(state->keyboard_fd, &event, sizeof(event))) ==
           (ssize_t)sizeof(event)) {
        if (event.type == state->down_type &&
            event.code == state->down_code &&
            event.value == state->down_value) {
            clock_gettime(CLOCK_MONOTONIC, &state->down_at);
            state->pressed = true;
        }

        if (event.type == state->up_type &&
            event.code == state->up_code &&
            event.value == state->up_value) {
            if (state->recording)
                finish_transmission(state);
            state->pressed = false;
        }
    }

    if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_error("Keyboard input failed: %s", strerror(errno));
        set_combined_status(state, "Transmitter keyboard error");
        return G_SOURCE_REMOVE;
    }

    if (state->pressed && !state->recording) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (timespec_diff_ms(&now, &state->down_at) >= state->threshold_ms)
            begin_transmission(state);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean quit_signal_cb(gpointer data)
{
    VisualPttState *state = data;
    g_application_quit(G_APPLICATION(state->rx.app));
    return G_SOURCE_REMOVE;
}

static gboolean initialize_transmitter(VisualPttState *state,
                                       const char *output_directory)
{
    const char *keyboard_device;
    const char *threshold;
    int length;

    state->keyboard_fd = -1;
    state->threshold_ms = 1000;
    state->config = ini_load("pttkey.ini");
    if (!state->config) {
        g_printerr("Cannot load pttkey.ini\n");
        return FALSE;
    }

    keyboard_device = ini_get(state->config, "pttkey", "keyboard_device");
    if (!keyboard_device || !*keyboard_device) {
        g_printerr("keyboard_device is not set in pttkey.ini\n");
        return FALSE;
    }

    state->down_type = config_int(state->config, "ptt_down_type", 1);
    state->down_code = config_int(state->config, "ptt_down_code", 100);
    state->down_value = config_int(state->config, "ptt_down_value", 1);
    state->up_type = config_int(state->config, "ptt_up_type", 1);
    state->up_code = config_int(state->config, "ptt_up_code", 100);
    state->up_value = config_int(state->config, "ptt_up_value", 0);
    threshold = ini_get(state->config, "pttkey", "ptt_down_threshold_ms");
    if (threshold && atoi(threshold) > 0)
        state->threshold_ms = atoi(threshold);
    state->start_wav = (char *)ini_get(state->config, "pttkey", "ptt_start_wav");
    state->end_wav = (char *)ini_get(state->config, "pttkey", "ptt_end_wav");

    if (ensure_output_dir(output_directory) != 0 ||
        make_absolute_path(output_directory, state->output_dir,
                           sizeof(state->output_dir)) != 0)
        return FALSE;

    length = g_snprintf(state->indicator_file,
                        sizeof(state->indicator_file), "%s/incoming.flag",
                        state->output_dir);
    if (length <= 0 || length >= (int)sizeof(state->indicator_file)) {
        g_printerr("Output indicator path is too long\n");
        return FALSE;
    }
    remove_indicator_file(state->indicator_file);

    state->keyboard_fd = open(keyboard_device, O_RDONLY | O_NONBLOCK);
    if (state->keyboard_fd < 0) {
        g_printerr("Cannot open keyboard %s: %s\n", keyboard_device,
                   strerror(errno));
        return FALSE;
    }
    log_info("Combined transmitter sends completed files to %s",
             state->output_dir);
    return TRUE;
}

static void combined_activate_cb(GtkApplication *app, gpointer data)
{
    VisualPttState *state = data;

    if (state->rx.window) {
        gtk_window_present(state->rx.window);
        return;
    }
    on_app_activate(app, &state->rx);
    if (state->keyboard_fd >= 0)
        g_timeout_add(10, transmitter_poll_cb, state);
    else
        set_combined_status(state, "Receiver active; transmitter unavailable");
}

static void cleanup_combined(VisualPttState *state)
{
    if (state->recording || state->pipeline)
        finish_transmission(state);
    if (state->rx.playbin) {
        gst_element_set_state(state->rx.playbin, GST_STATE_NULL);
        gst_object_unref(state->rx.playbin);
        state->rx.playbin = NULL;
    }
    remove_indicator_file(state->indicator_file);
    if (state->keyboard_fd >= 0)
        close(state->keyboard_fd);
    if (state->config)
        ini_free(state->config);
    if (state->rx.discoverer)
        g_object_unref(state->rx.discoverer);
    g_queue_free_full(state->rx.auto_queue, g_free);
    g_hash_table_unref(state->rx.known_files);
    if (state->rx.messages)
        g_object_unref(state->rx.messages);
}

int main(int argc, char **argv)
{
    VisualPttState state = {0};
    GtkApplication *app;
    GError *discoverer_error = NULL;
    struct stat input_info;
    int status;
    char *application_argv[] = { argv[0], NULL };
    int application_argc = 1;

    if (argc != 3) {
        g_printerr("Usage: %s INPUT_DIRECTORY OUTPUT_DIRECTORY\n", argv[0]);
        return 1;
    }
    if (stat(argv[1], &input_info) != 0 || !S_ISDIR(input_info.st_mode)) {
        g_printerr("Input directory '%s' is not accessible\n", argv[1]);
        return 1;
    }

    log_set_level(LOG_INFO);
    log_set_quiet(1);
    gst_init(&argc, &argv);

    g_strlcpy(state.rx.watch_dir, argv[1], sizeof(state.rx.watch_dir));
    if (g_snprintf(state.rx.indicator_path, sizeof(state.rx.indicator_path),
                   "%s/incoming.flag", state.rx.watch_dir) >=
        (int)sizeof(state.rx.indicator_path)) {
        g_printerr("Input indicator path is too long\n");
        return 1;
    }

    state.rx.known_files = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
    state.rx.auto_queue = g_queue_new();
    state.rx.discoverer = gst_discoverer_new(5 * GST_SECOND,
                                             &discoverer_error);
    if (!state.rx.discoverer) {
        g_printerr("Could not create media discoverer: %s\n",
                   discoverer_error ? discoverer_error->message : "unknown error");
        g_clear_error(&discoverer_error);
    }

    if (!initialize_transmitter(&state, argv[2])) {
        g_printerr("Could not initialize combined transmitter\n");
        cleanup_combined(&state);
        return 1;
    }
    app = gtk_application_new("org.resiliencetheatre.visualptt",
                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(combined_activate_cb), &state);
    g_unix_signal_add(SIGINT, quit_signal_cb, &state);
    g_unix_signal_add(SIGTERM, quit_signal_cb, &state);
    status = g_application_run(G_APPLICATION(app), application_argc,
                               application_argv);

    cleanup_combined(&state);
    g_object_unref(app);
    return status;
}
