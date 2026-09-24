#define main annotation_watcher_main
#include "../annotation_watcher.c"
#undef main
#include <assert.h>

static void script(const char *path, const char *body)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(body, file) >= 0 && fclose(file) == 0);
    assert(chmod(path, 0700) == 0);
}

int main(void)
{
    char root[] = "/tmp/visualptt-annotations-XXXXXX";
    assert(mkdtemp(root) && chdir(root) == 0);
    script("ffmpeg", "#!/bin/sh\nfor last do :; done\nprintf wav > \"$last\"\n");
    script("whisper", "#!/bin/sh\nprintf 'spoken words\\n'\n");
    script("printer", "#!/bin/sh\nprintf '%s' \"$3\" > printed\n");
    char stamp[128];
    const char *names[] = {"rec_20260923_091500.mkv", "rec_20260923_091500_0123456789abcdef0123456789abcdef.mkv",
        "A_rec_20260923_091500_0123456789abcdef0123456789abcdef.mkv"};
    for (size_t i = 0; i < sizeof names/sizeof *names; i++) {
        filename_timestamp(names[i], stamp, sizeof stamp);
        assert(!strcmp(stamp, "2026-09-23 09:15:00"));
        char *wav = replace_extension(names[i], ".wav");
        char *txt = replace_extension(names[i], ".txt");
        size_t stem = strlen(names[i]) - 4;
        assert(!strncmp(wav, names[i], stem) && !strcmp(wav+stem, ".wav"));
        assert(!strncmp(txt, names[i], stem) && !strcmp(txt+stem, ".txt"));
        assert(annotate(names[i], "./ffmpeg", "./whisper", "./printer", "18") == 0);
        assert(access(wav, F_OK) == 0 && access(txt, F_OK) == 0);
        char *printed = read_text_file("printed");
        assert(printed && !strcmp(printed, "VisualPTT message: 2026-09-23 09:15:00\nspoken words"));
        free(printed);
        unlink(wav); unlink(txt);
        free(wav); free(txt);
    }
    filename_timestamp("rec_20260923_091500.bad", stamp, sizeof stamp);
    assert(strcmp(stamp, "2026-09-23 09:15:00"));
    puts("annotation full-stem companions and printable timestamps passed");
    unlink("ffmpeg"); unlink("whisper"); unlink("printer"); unlink("printed");
    chdir("/"); rmdir(root);
    return 0;
}
