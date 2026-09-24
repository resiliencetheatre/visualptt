/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional VisualPTT collector: native Linux C, SQLite and built-in SHA-256.
 * See COLLECTOR.md for the durable intent/rename protocol and retention policy.
 */
#define _GNU_SOURCE
#include "recording.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include "sha256.h"
#include <signal.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TEMP_PREFIX ".visualptt-collector-"
#define TEMP_LENGTH (sizeof(TEMP_PREFIX) - 1 + 32 + 5)

typedef struct { dev_t dev; ino_t ino; } Identity;
typedef struct {
    char *path;
    char peer[33];
    int fd;
    Identity *ancestors;
    size_t depth;
} Directory;
typedef struct {
    char local[33];
    Directory inbox, state, own;
    Directory *sources;
    size_t source_count;
    double poll, retention, purge_interval, next_purge;
    bool purge_enabled, failed;
    int stable_scans;
    sqlite3_int64 scan_number;
    sqlite3 *db;
} Collector;
typedef struct Pending {
    char origin[33], name[128], temp[80], digest[65];
    sqlite3_int64 size;
    struct Pending *next;
} Pending;

static volatile sig_atomic_t running = 1;
#ifdef COLLECTOR_TESTING
/* Fault injection is compiled only into the native test executable. */
static void (*test_hook)(const char *stage);
#define CHECKPOINT(stage) do { if (test_hook) test_hook(stage); } while (0)
#else
#define CHECKPOINT(stage) ((void)0)
#endif

static void message(const char *level, const char *fmt, ...)
{
    int saved = errno;
    va_list args;
    fprintf(stderr, "visualptt-collector %s: ", level);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    errno = saved;
}

static bool valid_id(const char *id)
{
    size_t len = strlen(id);
    if (!len || len > 32) return false;
    for (size_t i = 0; i < len; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= 'A' && id[i] <= 'Z') ||
              (id[i] >= '0' && id[i] <= '9') || id[i] == '-')) return false;
    return true;
}

static bool valid_name(const char *name)
{
    return recording_timestamp_offset(name) == 4;
}

static bool hex_string(const char *s, size_t length)
{
    if (strlen(s) != length) return false;
    for (size_t i = 0; i < length; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

static bool valid_temp(const char *name)
{
    char hex[33];
    size_t prefix = sizeof(TEMP_PREFIX) - 1;
    if (strlen(name) != TEMP_LENGTH || strncmp(name, TEMP_PREFIX, prefix) ||
        strcmp(name + prefix + 32, ".part")) return false;
    memcpy(hex, name + prefix, 32); hex[32] = 0;
    return hex_string(hex, 32);
}

static bool regular(const struct stat *st)
{
    return S_ISREG(st->st_mode) && st->st_size > 0 && st->st_nlink == 1;
}

static bool same_stat(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
        a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

/* Format nanoseconds without overflowing when a filesystem supplies a time
 * outside the signed 64-bit nanosecond range. Preserve decimal ledger values. */
static void timestamp_number(struct timespec ts, char out[48])
{
    if (ts.tv_sec >= 0) {
        if (!ts.tv_sec) snprintf(out, 48, "%ld", ts.tv_nsec);
        else snprintf(out, 48, "%ju%09ld", (uintmax_t)ts.tv_sec, ts.tv_nsec);
    } else {
        uintmax_t seconds = (uintmax_t)(-(ts.tv_sec + 1));
        long nanos = 1000000000L - ts.tv_nsec;
        if (!ts.tv_nsec) { seconds++; nanos = 0; }
        if (!seconds) snprintf(out, 48, "-%ld", nanos);
        else snprintf(out, 48, "-%ju%09ld", seconds, nanos);
    }
}

/* Keep the existing ledger fingerprint representation for upgrade compatibility. */
static void fingerprint(const struct stat *st, char out[256])
{
    char mtime[48], ctime[48];
    timestamp_number(st->st_mtim, mtime); timestamp_number(st->st_ctim, ctime);
    snprintf(out, 256, "(%ju, %ju, %jd, %s, %s)", (uintmax_t)st->st_dev,
             (uintmax_t)st->st_ino, (intmax_t)st->st_size, mtime, ctime);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

static int setting_number(const char *s, double *out, double maximum)
{
    char *end;
    errno = 0;
    double value = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(value) || value <= 0 || value >= maximum) return -1;
    *out = value;
    return 0;
}

static int config_read(Collector *c, const char *filename)
{
    static const char *keys[] = { "local_peer", "input_dir", "state_dir", "poll_seconds",
        "stable_scans", "own_pool", "purge_enabled", "purge_interval_seconds", "retention_seconds" };
    FILE *file = fopen(filename, "r");
    if (!file) return -1;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    unsigned seen = 0, sections = 0, section = 0, lineno = 0;
    int result = -1;
    c->poll = 2; c->stable_scans = 1; c->retention = 172800; c->purge_interval = 900;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        lineno++;
        if ((size_t)length > PATH_MAX + 128 || memchr(line, 0, (size_t)length)) goto invalid;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            unsigned next = !strcmp(s, "[collector]") ? 1 : !strcmp(s, "[sources]") ? 2 : 0;
            if (!next || (sections & next)) goto invalid;
            sections |= next; section = next;
            continue;
        }
        char *equal = strchr(s, '=');
        if (!equal || !section) goto invalid;
        *equal = 0;
        char *key = trim(s), *value = trim(equal + 1);
        if (!*key || !*value) goto invalid;
        if (section == 2) {
            if (!valid_id(key)) goto invalid;
            for (size_t i = 0; i < c->source_count; i++)
                if (!strcmp(key, c->sources[i].peer)) goto invalid;
            Directory *items = realloc(c->sources, (c->source_count + 1) * sizeof(*items));
            if (!items) goto done;
            c->sources = items;
            Directory *d = &items[c->source_count++];
            *d = (Directory){.fd = -1};
            strcpy(d->peer, key);
            d->path = strdup(value);
            if (!d->path) goto done;
            continue;
        }
        size_t k;
        for (k = 0; k < sizeof(keys)/sizeof(*keys); k++) if (!strcmp(key, keys[k])) break;
        if (k == sizeof(keys)/sizeof(*keys) || (seen & (1u << k))) goto invalid;
        seen |= 1u << k;
        switch (k) {
        case 0:
            if (!valid_id(value)) goto invalid;
            strcpy(c->local, value); break;
        case 1: c->inbox.path = strdup(value); if (!c->inbox.path) goto done; break;
        case 2: c->state.path = strdup(value); if (!c->state.path) goto done; break;
        case 3: if (setting_number(value, &c->poll, 86400)) goto invalid; break;
        case 4:
            if (!strcmp(value, "1")) c->stable_scans = 1;
            else if (!strcmp(value, "2")) c->stable_scans = 2;
            else goto invalid;
            break;
        case 5: c->own.path = strdup(value); if (!c->own.path) goto done; break;
        case 6:
            if (!strcasecmp(value, "true") || !strcasecmp(value, "yes") ||
                !strcasecmp(value, "on") || !strcmp(value, "1")) c->purge_enabled = true;
            else if (!strcasecmp(value, "false") || !strcasecmp(value, "no") ||
                     !strcasecmp(value, "off") || !strcmp(value, "0")) c->purge_enabled = false;
            else goto invalid;
            break;
        case 7: if (setting_number(value, &c->purge_interval, 31536000)) goto invalid; break;
        case 8: if (setting_number(value, &c->retention, 315360000)) goto invalid; break;
        }
    }
    if (ferror(file)) goto done;
    if (sections != 3 || (seen & 7) != 7 || (c->purge_enabled && !c->own.path)) goto invalid;
    for (size_t i = 0; i < c->source_count; i++)
        if (!strcmp(c->local, c->sources[i].peer)) goto invalid;
    result = 0;
    goto done;
invalid:
    errno = EINVAL;
    message("ERROR", "Invalid/duplicate config setting, section, peer or required value near %s:%u", filename, lineno);
done:
    free(line);
    fclose(file);
    return result;
}

static int remember_ancestor(Directory *d)
{
    struct stat st;
    if (fstat(d->fd, &st)) return -1;
    Identity *items = realloc(d->ancestors, (d->depth + 1) * sizeof(*items));
    if (!items) return -1;
    d->ancestors = items;
    items[d->depth++] = (Identity){st.st_dev, st.st_ino};
    return 0;
}

static int directory_open(Directory *d, bool private)
{
    if (!d->path || d->path[0] != '/' || strlen(d->path) >= PATH_MAX) { errno = EINVAL; return -1; }
    char *copy = strdup(d->path), *save = NULL;
    if (!copy) return -1;
    char normalized[PATH_MAX] = "";
    d->fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (d->fd < 0 || remember_ancestor(d)) goto fail;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { errno = EINVAL; goto fail; }
        int child = openat(d->fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0) goto fail;
        close(d->fd); d->fd = child;
        if (remember_ancestor(d)) goto fail;
        strcat(normalized, "/"); strcat(normalized, part);
    }
    if (!normalized[0]) strcpy(normalized, "/");
    struct stat st;
    if (fstat(d->fd, &st)) goto fail;
    if (private && (st.st_uid != getuid() || (st.st_mode & 0022))) {
        errno = EACCES; goto fail;
    }
    free(copy);
    strcpy(d->path, normalized); /* Normalization can only shorten the original. */
    return 0;
fail:
    message("ERROR", "Cannot safely open %s: %s", d->path, strerror(errno));
    free(copy);
    return -1;
}

static bool same_identity(Identity a, Identity b) { return a.dev == b.dev && a.ino == b.ino; }

static bool overlapping(const Directory *a, const Directory *b)
{
    for (size_t i = 0; i < a->depth; i++)
        if (same_identity(a->ancestors[i], b->ancestors[b->depth - 1])) return true;
    for (size_t i = 0; i < b->depth; i++)
        if (same_identity(b->ancestors[i], a->ancestors[a->depth - 1])) return true;
    return false;
}

static Directory *directory_at(Collector *c, size_t n)
{
    if (n == 0) return &c->inbox;
    if (n == 1) return &c->state;
    if (n == 2) return &c->own;
    return &c->sources[n - 3];
}

static void directory_close(Directory *d)
{
    if (d->fd >= 0) close(d->fd);
    free(d->path); free(d->ancestors);
    *d = (Directory){.fd = -1};
}

static void collector_close(Collector *c)
{
    if (c->db) sqlite3_close(c->db);
    c->db = NULL;
    for (size_t i = 0; i < c->source_count + 3; i++) directory_close(directory_at(c, i));
    free(c->sources); c->sources = NULL; c->source_count = 0;
}

static void database_error(Collector *c)
{
    message("ERROR", "SQLite failure; stopping to preserve delivery state: %s", sqlite3_errmsg(c->db));
    c->failed = true;
    errno = EIO;
}

static int sql_exec(Collector *c, const char *sql)
{
    if (sqlite3_exec(c->db, sql, NULL, NULL, NULL) != SQLITE_OK) { database_error(c); return -1; }
    return 0;
}

static sqlite3_stmt *sql_prepare(Collector *c, const char *sql, int count, ...)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(c->db, sql, -1, &stmt, NULL) != SQLITE_OK) { database_error(c); return NULL; }
    va_list args;
    va_start(args, count);
    for (int i = 1; i <= count; i++) {
        const char *s = va_arg(args, const char *);
        if (sqlite3_bind_text(stmt, i, s, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            va_end(args); sqlite3_finalize(stmt); database_error(c); return NULL;
        }
    }
    va_end(args);
    return stmt;
}

static int sql_step(Collector *c, sqlite3_stmt *stmt)
{
    if (!stmt) return SQLITE_ERROR;
    int code = sqlite3_step(stmt);
    if (code != SQLITE_ROW && code != SQLITE_DONE) database_error(c);
    return code;
}

static int sql_write(Collector *c, sqlite3_stmt *stmt)
{
    int code = sql_step(c, stmt);
    sqlite3_finalize(stmt);
    return code == SQLITE_DONE ? 0 : -1;
}

/* Quoted representation used by the first collector's metadata binding.
 * Preserve it so switching implementation never requires discarding a ledger. */
static void binding_quote(sqlite3_str *out, const char *s)
{
    char quote = strchr(s, '\'') && !strchr(s, '"') ? '"' : '\'';
    sqlite3_str_appendchar(out, 1, quote);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == (unsigned char)quote || *p == '\\') sqlite3_str_appendchar(out, 1, '\\');
        if (*p == '\n') sqlite3_str_appendall(out, "\\n");
        else if (*p == '\r') sqlite3_str_appendall(out, "\\r");
        else if (*p == '\t') sqlite3_str_appendall(out, "\\t");
        else if (*p < 32 || *p == 127) sqlite3_str_appendf(out, "\\x%02x", *p);
        else sqlite3_str_appendchar(out, 1, (char)*p);
    }
    sqlite3_str_appendchar(out, 1, quote);
}

static char *binding(Collector *c)
{
    sqlite3_str *out = sqlite3_str_new(c->db);
    sqlite3_str_appendall(out, "("); binding_quote(out, c->local);
    sqlite3_str_appendall(out, ", [('input', "); binding_quote(out, c->inbox.path);
    if (c->own.path) {
        sqlite3_str_appendall(out, "), ('own', "); binding_quote(out, c->own.path);
    }
    sqlite3_str_appendall(out, "), ('state', "); binding_quote(out, c->state.path);
    sqlite3_str_appendall(out, ")])");
    return sqlite3_str_finish(out);
}

static int database_open(Collector *c)
{
    const char *files[] = {"ledger.sqlite3", "ledger.sqlite3-wal", "ledger.sqlite3-shm", "ledger.sqlite3-journal"};
    for (size_t i = 0; i < sizeof(files)/sizeof(*files); i++) {
        struct stat st;
        if (fstatat(c->state.fd, files[i], &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT) continue;
            return -1;
        }
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != getuid() || (st.st_mode & 0022)) { errno = EINVAL; return -1; }
    }
    char path[80];
    snprintf(path, sizeof path, "/proc/self/fd/%d/ledger.sqlite3", c->state.fd);
    if (sqlite3_open_v2(path, &c->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        database_error(c); return -1;
    }
    if (sql_exec(c, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY;"
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS delivery(origin TEXT NOT NULL, name TEXT NOT NULL, temp TEXT,"
        "size INTEGER NOT NULL, digest TEXT NOT NULL, PRIMARY KEY(origin,name));"
        "CREATE TABLE IF NOT EXISTS pool(name TEXT PRIMARY KEY, identity TEXT NOT NULL, observed REAL NOT NULL);"
        "CREATE INDEX IF NOT EXISTS delivery_pending ON delivery(temp) WHERE temp IS NOT NULL;"
        "CREATE TEMP TABLE stable(origin TEXT, name TEXT, identity TEXT, scan INTEGER, PRIMARY KEY(origin,name));")) return -1;
    char *expected = binding(c);
    if (!expected) { errno = ENOMEM; return -1; }
    sqlite3_stmt *stmt = sql_prepare(c, "SELECT value FROM metadata WHERE key='binding'", 0);
    int code = sql_step(c, stmt);
    bool mismatch = code == SQLITE_ROW && (!sqlite3_column_text(stmt, 0) ||
        strcmp((const char *)sqlite3_column_text(stmt, 0), expected));
    sqlite3_finalize(stmt);
    if (mismatch || (code != SQLITE_ROW && code != SQLITE_DONE)) {
        message("ERROR", "Ledger binding differs; preserve state and restore paths/peer ID or explicitly migrate");
        sqlite3_free(expected); errno = EINVAL; return -1;
    }
    int result = sql_write(c, sql_prepare(c, "INSERT OR IGNORE INTO metadata VALUES('binding',?)", 1, expected));
    sqlite3_free(expected);
    return result ? -1 : fsync(c->state.fd);
}

/* A new open file description is necessary: dup() would share directory offsets
 * and later scans would miss everything after the first readdir traversal. */
static DIR *directory_stream(int fd)
{
    int scanfd = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (scanfd < 0) return NULL;
    DIR *dir = fdopendir(scanfd);
    if (!dir) close(scanfd);
    return dir;
}

static int source_unchanged(int dir, const char *name, int fd, const struct stat *before)
{
    struct stat after, entry;
    if (fstat(fd, &after) || fstatat(dir, name, &entry, AT_SYMLINK_NOFOLLOW)) return -1;
    if (!regular(&after) || !regular(&entry) || !same_stat(before, &after) || !same_stat(before, &entry)) {
        errno = ESTALE; return -1;
    }
    return 0;
}

static int write_all(int fd, const void *data, size_t size)
{
    const unsigned char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += n; size -= (size_t)n;
    }
    return 0;
}

static int hash_stream(int in, int out, sqlite3_int64 *size, char hex[65])
{
    VisualPttSha256 md;
    unsigned char buffer[65536], digest[32];
    int result = -1;
    *size = 0;
    visualptt_sha256_init(&md);
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto done;
        if (!n) break;
        if (visualptt_sha256_update(&md, buffer, (size_t)n)) { errno = EFBIG; goto done; }
        if (out >= 0) {
            if (write_all(out, buffer, (size_t)n)) goto done;
            CHECKPOINT("copy");
        }
        if (*size > INT64_MAX - n) { errno = EFBIG; goto done; }
        *size += n;
    }
    visualptt_sha256_final(&md, digest);
    for (unsigned i = 0; i < sizeof digest; i++) sprintf(hex + i*2, "%02x", digest[i]);
    result = 0;
done:
    return result;
}

static int verify_file(int dir, const char *name, sqlite3_int64 size, const char *digest)
{
    CHECKPOINT("verify");
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    char hex[65];
    sqlite3_int64 count;
    int result = -1;
    if (fstat(fd, &st)) goto done;
    if (!regular(&st) || st.st_size != size) { errno = EEXIST; goto done; }
    if (hash_stream(fd, -1, &count, hex) || source_unchanged(dir, name, fd, &st)) goto done;
    if (count != size || strcmp(hex, digest)) { errno = EEXIST; goto done; }
    result = 0;
done:
    close(fd);
    return result;
}

static int delivery_done(Collector *c, const Pending *p)
{
    int result = sql_write(c, sql_prepare(c, "UPDATE delivery SET temp=NULL WHERE origin=? AND name=?", 2, p->origin, p->name));
    if (!result) CHECKPOINT("committed");
    return result;
}

static int publish(Collector *c, const Pending *p, bool recover)
{
    char target[192];
    snprintf(target, sizeof target, "%s_%s", p->origin, p->name);
    if (recover) {
        struct stat st;
        if (fstatat(c->inbox.fd, p->temp, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno != ENOENT) return -1;
            /* Intent was durable before rename. Missing temp means already
             * published, including a final file consumed before ledger commit. */
            if (fsync(c->inbox.fd) || delivery_done(c, p)) return -1;
            message("INFO", "Recovered published/consumed delivery %s/%s", p->origin, p->name);
            return 0;
        }
        if (verify_file(c->inbox.fd, p->temp, p->size, p->digest)) return -1;
    }
    if (renameat2(c->inbox.fd, p->temp, c->inbox.fd, target, RENAME_NOREPLACE)) {
        if (errno != EEXIST) return -1;
        if (verify_file(c->inbox.fd, target, p->size, p->digest)) {
            message("ERROR", "Destination collision/unsafe file: %s; pending copy retained", target);
            return -1;
        }
        if (unlinkat(c->inbox.fd, p->temp, 0)) return -1;
    }
    CHECKPOINT("rename");
    if (fsync(c->inbox.fd) || delivery_done(c, p)) return -1;
    message("INFO", "Delivered %s/%s", p->origin, p->name);
    return 0;
}

static int text_column(sqlite3_stmt *stmt, int column, char *out, size_t size)
{
    const unsigned char *s = sqlite3_column_text(stmt, column);
    int bytes = sqlite3_column_bytes(stmt, column);
    if (!s || bytes < 0 || (size_t)bytes >= size || strlen((const char *)s) != (size_t)bytes) return -1;
    memcpy(out, s, (size_t)bytes + 1);
    return 0;
}

static int recover_pending(Collector *c)
{
    Pending *head = NULL;
    sqlite3_stmt *stmt = sql_prepare(c, "SELECT origin,name,temp,size,digest FROM delivery WHERE temp IS NOT NULL", 0);
    int code;
    while ((code = sql_step(c, stmt)) == SQLITE_ROW) {
        Pending *p = calloc(1, sizeof(*p));
        if (!p) { c->failed = true; break; }
        p->next = head; head = p;
        p->size = sqlite3_column_int64(stmt, 3);
        if (text_column(stmt, 0, p->origin, sizeof p->origin) ||
            text_column(stmt, 1, p->name, sizeof p->name) || text_column(stmt, 2, p->temp, sizeof p->temp) ||
            text_column(stmt, 4, p->digest, sizeof p->digest) || !valid_id(p->origin) ||
            !valid_name(p->name) || !valid_temp(p->temp) || !hex_string(p->digest, 64) || p->size <= 0) {
            message("ERROR", "Invalid delivery journal row; refusing recovery");
            c->failed = true; break;
        }
    }
    sqlite3_finalize(stmt);
    while (head) {
        Pending *p = head; head = p->next;
        if (!c->failed && publish(c, p, true))
            message("ERROR", "Pending delivery %s/%s: %s", p->origin, p->name, strerror(errno));
        free(p);
    }
    return c->failed ? -1 : 0;
}

static int cleanup_orphans(Collector *c)
{
    DIR *dir = directory_stream(c->inbox.fd);
    if (!dir) return -1;
    struct dirent *entry;
    int result = 0;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) result = -1; break; }
        if (!valid_temp(entry->d_name)) continue;
        sqlite3_stmt *stmt = sql_prepare(c, "SELECT 1 FROM delivery WHERE temp=?", 1, entry->d_name);
        int code = sql_step(c, stmt);
        sqlite3_finalize(stmt);
        if (code == SQLITE_ROW) continue;
        if (code != SQLITE_DONE) { result = -1; break; }
        struct stat st;
        if (fstatat(c->inbox.fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { result = -1; break; }
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1) continue;
        if (unlinkat(c->inbox.fd, entry->d_name, 0)) { result = -1; break; }
        message("INFO", "Removed interrupted unjournaled copy %s", entry->d_name);
    }
    closedir(dir);
    return result ? -1 : fsync(c->inbox.fd);
}

static int collector_open(Collector *c, const char *config)
{
    *c = (Collector){.inbox.fd = -1, .state.fd = -1, .own.fd = -1};
    if (config_read(c, config)) goto fail;
    for (size_t i = 0; i < c->source_count + 3; i++) {
        Directory *d = directory_at(c, i);
        if (!d->path) continue;
        if (directory_open(d, i < 2)) goto fail;
        for (size_t j = 0; j < i; j++) {
            Directory *previous = directory_at(c, j);
            if (previous->path && overlapping(d, previous)) {
                message("ERROR", "Directories must be disjoint: %s and %s", previous->path, d->path);
                errno = EINVAL; goto fail;
            }
        }
    }
    if (flock(c->inbox.fd, LOCK_EX | LOCK_NB) || flock(c->state.fd, LOCK_EX | LOCK_NB)) {
        message("ERROR", "Inbox or state directory is already locked by a collector");
        goto fail;
    }
    if (database_open(c) || recover_pending(c) || cleanup_orphans(c)) goto fail;
    return 0;
fail:
    collector_close(c);
    return -1;
}

static int reserve_temp(int dir, char name[80])
{
    for (int attempt = 0; attempt < 128; attempt++) {
        unsigned char random[16];
        size_t used = 0;
        while (used < sizeof random) {
            ssize_t n = getrandom(random + used, sizeof random - used, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return -1;
            used += (size_t)n;
        }
        strcpy(name, TEMP_PREFIX);
        for (size_t i = 0; i < sizeof random; i++) sprintf(name + sizeof(TEMP_PREFIX) - 1 + 2*i, "%02x", random[i]);
        strcat(name, ".part");
        int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST; return -1;
}

static int copy_source(Collector *c, Directory *source, const char *name, const struct stat *observed)
{
    int in = -1, out = -1, result = -1;
    bool created = false, journaled = false;
    Pending p = {0};
    struct stat before;
    strcpy(p.origin, source->peer); strcpy(p.name, name);
    in = openat(source->fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (in < 0 || fstat(in, &before)) goto done;
    if (!regular(&before) || !same_stat(&before, observed)) { errno = ESTALE; goto done; }
    out = reserve_temp(c->inbox.fd, p.temp);
    if (out < 0) goto done;
    created = true;
    if (hash_stream(in, out, &p.size, p.digest) || source_unchanged(source->fd, name, in, &before)) goto done;
    if (p.size != before.st_size) { errno = ESTALE; goto done; }
    if (fsync(out)) goto done;
    int close_result = close(out); out = -1;
    if (close_result || fsync(c->inbox.fd)) goto done;
    CHECKPOINT("copied");
    char size[32]; snprintf(size, sizeof size, "%" PRId64, (int64_t)p.size);
    /* A failed commit may have an uncertain outcome. Preserve the temp and
     * stop on DB failure; startup determines whether it is journaled. */
    journaled = true;
    if (sql_write(c, sql_prepare(c, "INSERT INTO delivery VALUES(?,?,?,?,?)", 5,
                  p.origin, p.name, p.temp, size, p.digest))) goto done;
    CHECKPOINT("intent");
    result = publish(c, &p, false);
done:
    {
        int saved = errno;
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        if (created && !journaled && unlinkat(c->inbox.fd, p.temp, 0))
            message("ERROR", "Cannot remove failed copy %s: %s", p.temp, strerror(errno));
        errno = saved;
    }
    return result;
}

static int stable_source(Collector *c, Directory *source, const char *name, const struct stat *st)
{
    if (c->stable_scans == 1) return 1;
    char identity[256], scan[32];
    fingerprint(st, identity);
    snprintf(scan, sizeof scan, "%" PRId64, (int64_t)c->scan_number);
    sqlite3_stmt *stmt = sql_prepare(c, "SELECT identity,scan FROM stable WHERE origin=? AND name=?", 2, source->peer, name);
    int code = sql_step(c, stmt), result = 0;
    if (code == SQLITE_ROW) {
        const char *old = (const char *)sqlite3_column_text(stmt, 0);
        result = old && !strcmp(old, identity) && sqlite3_column_int64(stmt, 1) == c->scan_number - 1;
    }
    sqlite3_finalize(stmt);
    if (code != SQLITE_ROW && code != SQLITE_DONE) return -1;
    if (sql_write(c, sql_prepare(c, "INSERT OR REPLACE INTO stable VALUES(?,?,?,?)", 4, source->peer, name, identity, scan))) return -1;
    return result;
}

static int scan_sources(Collector *c)
{
    if (recover_pending(c)) return -1;
    c->scan_number++;
    for (size_t i = 0; i < c->source_count && !c->failed; i++) {
        Directory *source = &c->sources[i];
        DIR *dir = directory_stream(source->fd);
        if (!dir) { message("ERROR", "Cannot scan %s: %s", source->peer, strerror(errno)); continue; }
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(dir);
            if (!entry) { if (errno) message("ERROR", "Scan failed for %s: %s", source->peer, strerror(errno)); break; }
            const char *name = entry->d_name;
            if (!valid_name(name)) continue;
            sqlite3_stmt *stmt = sql_prepare(c, "SELECT 1 FROM delivery WHERE origin=? AND name=?", 2, source->peer, name);
            int code = sql_step(c, stmt); sqlite3_finalize(stmt);
            if (code == SQLITE_ROW) continue;
            if (code != SQLITE_DONE) break;
            struct stat st;
            if (fstatat(source->fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
                message("ERROR", "Cannot stat %s/%s: %s", source->peer, name, strerror(errno)); continue;
            }
            if (!regular(&st)) { message("WARNING", "Rejecting unsafe/empty source %s/%s", source->peer, name); continue; }
            int ready = stable_source(c, source, name, &st);
            if (ready < 0) break;
            if (ready && copy_source(c, source, name, &st))
                message("ERROR", "Cannot deliver %s/%s: %s", source->peer, name, strerror(errno));
            if (c->failed) break;
        }
        closedir(dir);
    }
    if (c->failed) return -1;
    char scan[32]; snprintf(scan, sizeof scan, "%" PRId64, (int64_t)c->scan_number);
    return sql_write(c, sql_prepare(c, "DELETE FROM stable WHERE scan<>?", 1, scan));
}

static int purge_file(Collector *c, const char *name, double now)
{
    int fd = openat(c->own.fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = -1;
    struct stat st;
    char identity[256], observed[64];
    if (fstat(fd, &st)) goto done;
    if (!regular(&st)) { errno = EINVAL; goto done; }
    if (flock(fd, LOCK_EX | LOCK_NB)) goto done;
    fingerprint(&st, identity);
    sqlite3_stmt *stmt = sql_prepare(c, "SELECT identity,observed FROM pool WHERE name=?", 1, name);
    int code = sql_step(c, stmt);
    bool reset = code == SQLITE_DONE;
    double since = now;
    if (code == SQLITE_ROW) {
        const char *old = (const char *)sqlite3_column_text(stmt, 0);
        since = sqlite3_column_double(stmt, 1);
        reset = !old || strcmp(old, identity) || !isfinite(since) || now < since;
    }
    sqlite3_finalize(stmt);
    if (code != SQLITE_ROW && code != SQLITE_DONE) goto done;
    if (reset) {
        snprintf(observed, sizeof observed, "%.17g", now);
        result = sql_write(c, sql_prepare(c, "INSERT OR REPLACE INTO pool VALUES(?,?,?)", 3, name, identity, observed));
        goto done;
    }
    if (now - since < c->retention) { result = 0; goto done; }
    if (source_unchanged(c->own.fd, name, fd, &st)) goto done;
    if (unlinkat(c->own.fd, name, 0)) goto done;
    message("INFO", "Purged own pool recording %s", name);
    if (fsync(c->own.fd)) goto done;
    result = sql_write(c, sql_prepare(c, "DELETE FROM pool WHERE name=?", 1, name));
done:
    close(fd);
    return result;
}

static int purge_pool(Collector *c, double now)
{
    if (!c->purge_enabled) return 0;
    DIR *dir = directory_stream(c->own.fd);
    if (!dir) { message("ERROR", "Own pool scan failed: %s", strerror(errno)); return -1; }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) { message("ERROR", "Own pool scan failed: %s", strerror(errno)); result = -1; } break; }
        if (!valid_name(entry->d_name)) continue;
        if (purge_file(c, entry->d_name, now)) {
            message("ERROR", "Purge failed for %s (retained unless deletion was already logged): %s", entry->d_name, strerror(errno));
            result = -1;
        }
        if (c->failed) break;
    }
    closedir(dir);
    return result;
}

static double monotonic_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void stop_signal(int sig) { (void)sig; running = 0; }

static void usage(FILE *out)
{
    fprintf(out, "Usage: visualptt-collector --config FILE [--once]\n"
                 "Copy remote VisualPTT pools to a private inbox with a durable SQLite ledger.\n"
                 "  --once  Recover and scan once (stable_scans=2 needs a running daemon).\n");
}

int main(int argc, char **argv)
{
    const char *config = NULL;
    bool once = false;
    static const struct option options[] = {{"config", required_argument, NULL, 'c'},
        {"once", no_argument, NULL, '1'}, {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}};
    int option;
    while ((option = getopt_long(argc, argv, "c:h", options, NULL)) != -1) {
        if (option == 'c') config = optarg;
        else if (option == '1') once = true;
        else if (option == 'h') { usage(stdout); return 0; }
        else { usage(stderr); return 1; }
    }
    if (!config || optind != argc) { usage(stderr); return 1; }
    umask(0077);
    struct sigaction action = {.sa_handler = stop_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    Collector c;
    if (collector_open(&c, config)) { message("ERROR", "Initialization failed: %s", strerror(errno)); return 1; }
    int result = 0;
    while (running) {
        if (scan_sources(&c)) { result = 1; break; }
        if (c.purge_enabled && monotonic_seconds() >= c.next_purge) {
            purge_pool(&c, (double)time(NULL));
            c.next_purge = monotonic_seconds() + c.purge_interval;
            if (c.failed) { result = 1; break; }
        }
        if (once) break;
        double deadline = monotonic_seconds() + c.poll;
        while (running && monotonic_seconds() < deadline) {
            double delay = deadline - monotonic_seconds();
            if (delay > 0.2) delay = 0.2;
            if (delay <= 0) break;
            struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)(delay * 1e9)};
            nanosleep(&ts, NULL);
        }
    }
    collector_close(&c);
    return result;
}
