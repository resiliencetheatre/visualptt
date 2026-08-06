/*
 * annotation-watcher - generate annotations for completed media files.
 * Copyright (C) 2025 Resilience Theatre
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

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DIRECTORY "samples"
#define DEFAULT_FFMPEG "/usr/bin/ffmpeg"
#define DEFAULT_WHISPER "/usr/local/bin/whisper-cli"
#define DEFAULT_PRINT_FONT_SIZE "18"
#define DEFAULT_INTERVAL 2
#define RETRY_DELAY 30

struct file_state {
    char *name;
    off_t size;
    time_t mtime;
    unsigned int stable_scans;
    time_t last_attempt;
    struct file_state *next;
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [-d directory] [-i seconds] [-F ffmpeg] [-W whisper-cli]\n"
            "          [-P printer-program] [-S print-font-size]\n"
            "\n"
            "Defaults:\n"
            "  directory:   %s\n"
            "  interval:    %d seconds\n"
            "  ffmpeg:      %s\n"
            "  whisper-cli: %s\n"
            "  printer:     disabled\n"
            "  print font:  %s\n",
            program, DEFAULT_DIRECTORY, DEFAULT_INTERVAL, DEFAULT_FFMPEG,
            DEFAULT_WHISPER, DEFAULT_PRINT_FONT_SIZE);
}

static bool has_mkv_extension(const char *name)
{
    size_t length = strlen(name);
    return length >= 4 && strcasecmp(name + length - 4, ".mkv") == 0;
}

static char *join_path(const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    bool separator = directory_length > 0 && directory[directory_length - 1] != '/';
    char *path = malloc(directory_length + separator + name_length + 1);

    if (path == NULL)
        return NULL;
    memcpy(path, directory, directory_length);
    if (separator)
        path[directory_length++] = '/';
    memcpy(path + directory_length, name, name_length + 1);
    return path;
}

static char *replace_extension(const char *path, const char *extension)
{
    size_t length = strlen(path);
    size_t extension_length = strlen(extension);
    char *result;

    if (length < 4)
        return NULL;
    result = malloc(length - 4 + extension_length + 1);
    if (result == NULL)
        return NULL;
    memcpy(result, path, length - 4);
    memcpy(result + length - 4, extension, extension_length + 1);
    return result;
}

static char *append_suffix(const char *path, const char *suffix)
{
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);
    char *result = malloc(path_length + suffix_length + 1);

    if (result == NULL)
        return NULL;
    memcpy(result, path, path_length);
    memcpy(result + path_length, suffix, suffix_length + 1);
    return result;
}

static int run_process(char *const arguments[], const char *stdout_path)
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        if (stdout_path != NULL) {
            FILE *output = fopen(stdout_path, "w");
            if (output == NULL) {
                perror(stdout_path);
                _exit(126);
            }
            if (dup2(fileno(output), STDOUT_FILENO) < 0) {
                perror("dup2");
                _exit(126);
            }
            fclose(output);
        }
        execv(arguments[0], arguments);
        perror(arguments[0]);
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        fprintf(stderr, "%s terminated by signal %d\n", arguments[0],
                WTERMSIG(status));
    return -1;
}

static char *read_text_file(const char *path)
{
    FILE *stream = fopen(path, "r");
    char *contents;
    long length;

    if (stream == NULL)
        return NULL;
    if (fseek(stream, 0, SEEK_END) != 0 || (length = ftell(stream)) < 0 ||
        fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return NULL;
    }
    contents = malloc((size_t)length + 1);
    if (contents == NULL) {
        fclose(stream);
        return NULL;
    }
    if (fread(contents, 1, (size_t)length, stream) != (size_t)length) {
        free(contents);
        fclose(stream);
        return NULL;
    }
    contents[length] = '\0';
    fclose(stream);
    return contents;
}

static void collapse_to_one_line(char *text)
{
    char *read = text;
    char *write = text;
    bool pending_space = false;

    while (*read != '\0') {
        if (*read == ' ' || *read == '\t' || *read == '\r' || *read == '\n') {
            if (write != text)
                pending_space = true;
        } else {
            if (pending_space)
                *write++ = ' ';
            *write++ = *read;
            pending_space = false;
        }
        read++;
    }
    *write = '\0';
}

static void filename_timestamp(const char *input, char *timestamp, size_t size)
{
    const char *name = strrchr(input, '/');
    int year, month, day, hour, minute, second;
    char trailing;

    name = name == NULL ? input : name + 1;
    if (sscanf(name, "rec_%4d%2d%2d_%2d%2d%2d.mkv%c", &year, &month, &day,
               &hour, &minute, &second, &trailing) == 6) {
        snprintf(timestamp, size, "%04d-%02d-%02d %02d:%02d:%02d",
                 year, month, day, hour, minute, second);
        return;
    }
    snprintf(timestamp, size, "%s", name);
    char *extension = strrchr(timestamp, '.');
    if (extension != NULL && strcasecmp(extension, ".mkv") == 0)
        *extension = '\0';
}

static void print_annotation(const char *input, const char *text_path,
                             const char *printer, const char *font_size)
{
    char timestamp[128];
    char *annotation = read_text_file(text_path);
    char *message;
    size_t message_size;
    int status;

    if (annotation == NULL) {
        fprintf(stderr, "Could not read annotation for printing: %s\n", text_path);
        return;
    }
    collapse_to_one_line(annotation);
    filename_timestamp(input, timestamp, sizeof(timestamp));
    message_size = strlen("VisualPTT message: \n") + strlen(timestamp) +
                   strlen(annotation) + 1;
    message = malloc(message_size);
    if (message == NULL) {
        fprintf(stderr, "Out of memory while preparing print for %s\n", input);
        free(annotation);
        return;
    }
    snprintf(message, message_size, "VisualPTT message: %s\n%s", timestamp,
             annotation);

    char *printer_arguments[] = {(char *)printer, "--font-size",
                                 (char *)font_size, message, NULL};
    status = run_process(printer_arguments, NULL);
    if (status != 0)
        fprintf(stderr, "Printer program failed for %s (status %d); annotation retained\n",
                input, status);
    else
        fprintf(stderr, "Printed annotation: %s\n", text_path);

    free(message);
    free(annotation);
}

static int annotate(const char *input, const char *ffmpeg, const char *whisper,
                    const char *printer, const char *font_size)
{
    char *wav = replace_extension(input, ".wav");
    char *text = replace_extension(input, ".txt");
    char *wav_temp = NULL;
    char *text_temp = NULL;
    int result = -1;

    if (wav == NULL || text == NULL ||
        (wav_temp = append_suffix(wav, ".part")) == NULL ||
        (text_temp = append_suffix(text, ".part")) == NULL) {
        fprintf(stderr, "Out of memory while preparing %s\n", input);
        goto done;
    }

    fprintf(stderr, "Converting: %s -> %s\n", input, wav);
    char *ffmpeg_arguments[] = {(char *)ffmpeg, "-nostdin", "-loglevel", "error",
                                "-y", "-i", (char *)input, "-vn", "-ac", "1",
                                "-ar", "16000", "-c:a", "pcm_s16le", "-f",
                                "wav", wav_temp, NULL};
    if (run_process(ffmpeg_arguments, NULL) != 0) {
        fprintf(stderr, "Audio conversion failed for %s\n", input);
        unlink(wav_temp);
        goto done;
    }
    if (rename(wav_temp, wav) < 0) {
        perror("rename wav");
        unlink(wav_temp);
        goto done;
    }

    fprintf(stderr, "Transcribing: %s -> %s\n", wav, text);
    char *whisper_arguments[] = {(char *)whisper, "-nt", "-f", wav, NULL};
    if (run_process(whisper_arguments, text_temp) != 0) {
        fprintf(stderr, "Transcription failed for %s\n", input);
        unlink(text_temp);
        goto done;
    }
    if (rename(text_temp, text) < 0) {
        perror("rename transcript");
        unlink(text_temp);
        goto done;
    }
    fprintf(stderr, "Annotated: %s\n", text);
    if (printer != NULL)
        print_annotation(input, text, printer, font_size);
    result = 0;

done:
    free(wav);
    free(text);
    free(wav_temp);
    free(text_temp);
    return result;
}

static struct file_state *find_state(struct file_state **states, const char *name)
{
    struct file_state *state;

    for (state = *states; state != NULL; state = state->next) {
        if (strcmp(state->name, name) == 0)
            return state;
    }
    state = calloc(1, sizeof(*state));
    if (state == NULL)
        return NULL;
    state->name = strdup(name);
    if (state->name == NULL) {
        free(state);
        return NULL;
    }
    state->next = *states;
    *states = state;
    return state;
}

static void scan_directory(const char *directory, const char *ffmpeg,
                           const char *whisper, const char *printer,
                           const char *font_size, struct file_state **states)
{
    DIR *stream = opendir(directory);
    struct dirent *entry;

    if (stream == NULL) {
        perror(directory);
        return;
    }
    while ((entry = readdir(stream)) != NULL && !stop_requested) {
        struct stat information;
        struct file_state *state;
        char *input;
        char *text;
        time_t now;

        if (!has_mkv_extension(entry->d_name))
            continue;
        input = join_path(directory, entry->d_name);
        if (input == NULL)
            continue;
        if (stat(input, &information) < 0 || !S_ISREG(information.st_mode)) {
            free(input);
            continue;
        }
        state = find_state(states, entry->d_name);
        if (state == NULL) {
            fprintf(stderr, "Out of memory while tracking %s\n", input);
            free(input);
            continue;
        }
        if (state->size != information.st_size || state->mtime != information.st_mtime) {
            state->size = information.st_size;
            state->mtime = information.st_mtime;
            state->stable_scans = 0;
            free(input);
            continue;
        }
        if (state->stable_scans < 2)
            state->stable_scans++;
        if (state->stable_scans < 1) {
            free(input);
            continue;
        }

        text = replace_extension(input, ".txt");
        if (text == NULL) {
            free(input);
            continue;
        }
        if (access(text, F_OK) == 0) {
            free(text);
            free(input);
            continue;
        }
        now = time(NULL);
        if (state->last_attempt == 0 || now - state->last_attempt >= RETRY_DELAY) {
            state->last_attempt = now;
            annotate(input, ffmpeg, whisper, printer, font_size);
        }
        free(text);
        free(input);
    }
    closedir(stream);
}

static void free_states(struct file_state *states)
{
    while (states != NULL) {
        struct file_state *next = states->next;
        free(states->name);
        free(states);
        states = next;
    }
}

int main(int argc, char **argv)
{
    const char *directory = DEFAULT_DIRECTORY;
    const char *ffmpeg = DEFAULT_FFMPEG;
    const char *whisper = DEFAULT_WHISPER;
    const char *printer = NULL;
    const char *font_size = DEFAULT_PRINT_FONT_SIZE;
    unsigned int interval = DEFAULT_INTERVAL;
    struct file_state *states = NULL;
    struct sigaction action = {0};
    int option;

    while ((option = getopt(argc, argv, "d:i:F:W:P:S:h")) != -1) {
        switch (option) {
        case 'd': directory = optarg; break;
        case 'i': {
            char *end;
            unsigned long value = strtoul(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || value == 0 || value > 3600) {
                fprintf(stderr, "Invalid polling interval: %s\n", optarg);
                return EXIT_FAILURE;
            }
            interval = (unsigned int)value;
            break;
        }
        case 'F': ffmpeg = optarg; break;
        case 'W': whisper = optarg; break;
        case 'P': printer = optarg; break;
        case 'S': {
            char *end;
            unsigned long value = strtoul(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || value == 0 || value > 1000) {
                fprintf(stderr, "Invalid print font size: %s\n", optarg);
                return EXIT_FAILURE;
            }
            font_size = optarg;
            break;
        }
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind != argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    fprintf(stderr, "Watching %s for MKV files (Ctrl-C to stop)\n", directory);
    while (!stop_requested) {
        struct timespec delay = {(time_t)interval, 0};
        scan_directory(directory, ffmpeg, whisper, printer, font_size, &states);
        while (!stop_requested && nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
    }
    free_states(states);
    fprintf(stderr, "Stopped.\n");
    return EXIT_SUCCESS;
}
