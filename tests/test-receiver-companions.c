#define main receiver_standalone_main
#include "../visualptt-rx-list.c"
#undef main
#include <assert.h>

int main(void)
{
    char root[] = "/tmp/visualptt-receiver-XXXXXX";
    assert(mkdtemp(root) && chdir(root) == 0);
    const char *names[] = { "rec_20260923_091500.mkv",
        "rec_20260923_091500_0123456789abcdef0123456789abcdef.mkv",
        "desk_rec_20260923_091500_0123456789abcdef0123456789abcdef.mkv" };
    AppState state = {0};
    state.annotation_buffer = gtk_text_buffer_new(NULL);
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        char txt[160], wav[160];
        snprintf(txt, sizeof txt, "%.*s.txt", (int)strlen(names[i])-4, names[i]);
        snprintf(wav, sizeof wav, "%.*s.wav", (int)strlen(names[i])-4, names[i]);
        assert(g_file_set_contents(txt, "transcript", -1, NULL));
        assert(g_file_set_contents(wav, "audio", -1, NULL));
        refresh_annotation(&state, names[i], TRUE);
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(state.annotation_buffer, &start, &end);
        gchar *text = gtk_text_buffer_get_text(state.annotation_buffer, &start, &end, FALSE);
        assert(strstr(text, "transcript"));
        g_free(text);
        assert(delete_annotation_files(names[i]) == 0);
        assert(access(txt, F_OK) != 0 && access(wav, F_OK) != 0);
    }
    g_object_unref(state.annotation_buffer);
    chdir("/"); rmdir(root);
    puts("receiver finds/displays/deletes full-stem legacy/new/origin companions");
    return 0;
}
