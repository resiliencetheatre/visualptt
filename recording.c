#define _GNU_SOURCE
#include "recording.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>

static int random_hex(char out[33])
{
    unsigned char bytes[16];
    size_t used = 0;
    while (used < sizeof bytes) {
        ssize_t n = getrandom(bytes + used, sizeof bytes - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += n;
    }
    for (size_t i = 0; i < sizeof bytes; i++)
        sprintf(out + 2*i, "%02x", bytes[i]);
    return 0;
}

int recording_reserve(char *name, size_t namesize, char *path, size_t pathsize)
{
    char stamp[32], hex[33], cwd[PATH_MAX];
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) || !getcwd(cwd, sizeof cwd) ||
        !strftime(stamp, sizeof stamp, "rec_%Y%m%d_%H%M%S", &tm)) return -1;
    for (int attempt = 0; attempt < 128; attempt++) {
        if (random_hex(hex)) return -1;
        int n = snprintf(name, namesize, "%s_%s.mkv", stamp, hex);
        int p = snprintf(path, pathsize, "%s/.%s.part", cwd, name);
        if (n < 0 || (size_t)n >= namesize || p < 0 || (size_t)p >= pathsize) {
            errno = ENAMETOOLONG; return -1;
        }
        int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int recording_publish(const char *source, const char *name, const char *directory)
{
    int in = -1, out = -1, dir = -1, result = -1, saved, owned_temp = 0;
    char temp[96] = "", hex[33], buf[65536];
    struct stat before, after;
    if (!directory || !*directory) directory = ".";
    if (strchr(name, '/') || recording_timestamp_offset(name) != 4) {
        errno = EINVAL; return -1;
    }
    dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) goto done;
    in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0 || fstat(in, &before) || !S_ISREG(before.st_mode) || before.st_size <= 0) {
        errno = EINVAL; goto done;
    }
    for (int i = 0; i < 128; i++) {
        if (random_hex(hex)) goto done;
        snprintf(temp, sizeof temp, ".visualptt-%s.part", hex);
        out = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (out >= 0) { owned_temp = 1; break; }
        if (errno != EEXIST) goto done;
        temp[0] = 0;
    }
    if (out < 0) goto done;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) break;
        for (ssize_t pos = 0; pos < n;) {
            ssize_t w = write(out, buf + pos, n - pos);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) goto done;
            pos += w;
        }
    }
    if (fstat(in, &after) || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) { errno = ESTALE; goto done; }
    if (fsync(out)) goto done;
    if (close(out)) { out = -1; goto done; }
    out = -1;
    if (renameat2(dir, temp, dir, name, RENAME_NOREPLACE)) goto done;
    temp[0] = 0;
    if (fsync(dir)) goto done;
    /* On any publication failure the original remains available for recovery. */
    if (unlink(source)) goto done;
    result = 0;
done:
    saved = errno;
    if (out >= 0) close(out);
    if (dir >= 0 && owned_temp && temp[0]) unlinkat(dir, temp, 0);
    if (in >= 0) close(in);
    if (dir >= 0) close(dir);
    errno = saved;
    return result;
}

int recording_timestamp_offset(const char *name)
{
    const char *rec = name;
    if (strncmp(rec, "rec_", 4) || (rec[4] < '0' || rec[4] > '9')) {
        /* Origin IDs contain ASCII alphanumerics and hyphens only, max 32. */
        size_t n = strcspn(rec, "_");
        if (!n || n > 32 || rec[n] != '_') return -1;
        for (size_t i = 0; i < n; i++)
            if (!((rec[i] >= 'A' && rec[i] <= 'Z') ||
                  (rec[i] >= 'a' && rec[i] <= 'z') ||
                  (rec[i] >= '0' && rec[i] <= '9') || rec[i] == '-')) return -1;
        rec += n + 1;
    }
    if (strlen(rec) < 23 || strncmp(rec, "rec_", 4)) return -1;
    for (int i = 4; i < 19; i++) {
        if (i == 12) { if (rec[i] != '_') return -1; }
        else if (rec[i] < '0' || rec[i] > '9') return -1;
    }
    const char *end = rec + 19;
    if (*end == '_') {
        const char *hex = ++end;
        while ((*end >= '0' && *end <= '9') || (*end >= 'a' && *end <= 'f')) end++;
        if (end - hex < 32 || end - hex > 64 || (end - hex) % 2) return -1;
    }
    if (strcmp(end, ".mkv")) return -1;
    int year, month, day, hour, minute, second;
    if (sscanf(rec + 4, "%4d%2d%2d_%2d%2d%2d", &year, &month, &day,
               &hour, &minute, &second) != 6) return -1;
    const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 1 || month < 1 || month > 12 || day < 1 ||
        day > days[month] + (month == 2 && year%4 == 0 && (year%100 != 0 || year%400 == 0)) ||
        hour > 23 || minute > 59 || second > 59) return -1;
    return (int)(rec - name) + 4;
}
