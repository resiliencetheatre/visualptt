#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <openssl/evp.h>
#include <strophe.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ini.h"

extern char **environ;

#define UPLOAD_NS "urn:xmpp:http:upload:0"
#define DISCO_INFO_NS "http://jabber.org/protocol/disco#info"
#define DISCO_ITEMS_NS "http://jabber.org/protocol/disco#items"
#define OOB_NS "jabber:x:oob"
#define SFS_NS "urn:xmpp:sfs:0"
#define FILE_METADATA_NS "urn:xmpp:file:metadata:0"
#define URL_DATA_NS "http://jabber.org/protocol/url-data"
#define HASHES_NS "urn:xmpp:hashes:2"
#define DEFAULT_TIMEOUT 30L

enum exit_code {
    EXIT_USAGE_ERROR = 2,
    EXIT_CONFIG_ERROR = 3,
    EXIT_INPUT_ERROR = 4,
    EXIT_CONVERT_ERROR = 5,
    EXIT_CONNECT_ERROR = 6,
    EXIT_AUTH_ERROR = 7,
    EXIT_DISCOVERY_ERROR = 8,
    EXIT_SLOT_ERROR = 9,
    EXIT_HTTP_ERROR = 10,
    EXIT_MESSAGE_ERROR = 11
};

struct options {
    const char *to;
    const char *annotation;
    const char *config;
    const char *input;
    const char *message;
    const char *ffmpeg;
    long timeout;
    bool keep;
    bool debug;
};

struct config {
    char *jid;
    char *password;
    char *server;
    char *resource;
    unsigned short port;
};

struct app {
    xmpp_ctx_t *ctx;
    xmpp_conn_t *conn;
    struct options opt;
    struct config cfg;
    const char *mp4;
    const char *filename;
    off_t file_size;
    char *domain;
    char *body_prefix;
    char *sha256;
    char *upload_service;
    int next_id;
    int info_pending;
    bool items_done;
    bool slot_requested;
    bool raw_connected;
    bool connected_once;
    bool finished;
    int result;
    time_t deadline;
};

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

static void errorf(const char *message)
{
    fprintf(stderr, "ERROR: %s\n", message);
}

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: visualptt-xmpp-send [OPTIONS] --to JID RECORDING.mkv\n"
        "Convert a VisualPTT recording, upload it with XEP-0363, and send its URL.\n\n"
        "  --to JID             recipient JID (required)\n"
        "  --annotation FILE    text included with the attachment\n"
        "  --config FILE        XMPP INI file (default: ~/.config/visualptt/xmpp.ini)\n"
        "  --keep-converted     retain and print the temporary MP4 path\n"
        "  --message TEXT       message heading (default: VisualPTT message)\n"
        "  --ffmpeg PATH        ffmpeg executable (default: ffmpeg)\n"
        "  --timeout SECONDS    network/ffmpeg timeout (default: 30)\n"
        "  --debug              show protocol diagnostics (never credentials)\n"
        "  --help               show this help\n");
}

static int parse_options(int argc, char **argv, struct options *opt)
{
    static const struct option long_options[] = {
        {"to", required_argument, NULL, 't'},
        {"annotation", required_argument, NULL, 'a'},
        {"config", required_argument, NULL, 'c'},
        {"keep-converted", no_argument, NULL, 'k'},
        {"message", required_argument, NULL, 'm'},
        {"ffmpeg", required_argument, NULL, 'f'},
        {"timeout", required_argument, NULL, 'T'},
        {"debug", no_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int ch;
    char *end;

    memset(opt, 0, sizeof(*opt));
    opt->ffmpeg = "ffmpeg";
    opt->message = "VisualPTT message";
    opt->timeout = DEFAULT_TIMEOUT;
    while ((ch = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (ch) {
        case 't': opt->to = optarg; break;
        case 'a': opt->annotation = optarg; break;
        case 'c': opt->config = optarg; break;
        case 'k': opt->keep = true; break;
        case 'm': opt->message = optarg; break;
        case 'f': opt->ffmpeg = optarg; break;
        case 'd': opt->debug = true; break;
        case 'h': usage(stdout); return 1;
        case 'T':
            errno = 0;
            opt->timeout = strtol(optarg, &end, 10);
            if (errno || *end || opt->timeout < 1 || opt->timeout > 3600) {
                errorf("--timeout must be between 1 and 3600 seconds");
                return -1;
            }
            break;
        default: return -1;
        }
    }
    if (!opt->to || optind != argc - 1) {
        errorf("--to and exactly one MKV recording are required");
        usage(stderr);
        return -1;
    }
    opt->input = argv[optind];
    return 0;
}

static char *default_config_path(void)
{
    const char *home = getenv("HOME");
    char *path;
    size_t size;
    if (!home || !*home) return NULL;
    size = strlen(home) + strlen("/.config/visualptt/xmpp.ini") + 1;
    path = malloc(size);
    if (path) snprintf(path, size, "%s/.config/visualptt/xmpp.ini", home);
    return path;
}

static char *copy_value(ini_t *ini, const char *key)
{
    const char *value = ini_get(ini, "xmpp", key);
    return value && *value ? strdup(value) : NULL;
}

static void config_free(struct config *cfg)
{
    if (cfg->password) memset(cfg->password, 0, strlen(cfg->password));
    free(cfg->jid); free(cfg->password); free(cfg->server); free(cfg->resource);
    memset(cfg, 0, sizeof(*cfg));
}

static int load_config(const char *path, struct config *cfg)
{
    struct stat st;
    ini_t *ini;
    const char *port_text;
    char *end;
    long port = 5222;

    if (stat(path, &st) < 0) {
        fprintf(stderr, "ERROR: configuration file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (st.st_mode & (S_IRGRP | S_IROTH))
        fprintf(stderr, "WARNING: configuration file containing a password is readable by group or others\n");
    ini = ini_load(path);
    if (!ini) { errorf("could not read configuration file"); return -1; }
    cfg->jid = copy_value(ini, "jid");
    cfg->password = copy_value(ini, "password");
    cfg->server = copy_value(ini, "server");
    cfg->resource = copy_value(ini, "resource");
    port_text = ini_get(ini, "xmpp", "port");
    if (port_text) {
        errno = 0;
        port = strtol(port_text, &end, 10);
        if (errno || *end || port < 1 || port > 65535) port = -1;
    }
    ini_free(ini);
    if (!cfg->jid || !cfg->password || !cfg->server || port < 0 ||
        !strchr(cfg->jid, '@')) {
        errorf("malformed configuration: [xmpp] requires valid jid, password, server, and optional port");
        return -1;
    }
    if (!cfg->resource) cfg->resource = strdup("visualptt");
    if (!cfg->resource) { errorf("out of memory"); return -1; }
    cfg->port = (unsigned short)port;
    return 0;
}

static int validate_regular_file(const char *path, const char *description)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "ERROR: %s '%s': %s\n", description, path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode) || access(path, R_OK) < 0) {
        fprintf(stderr, "ERROR: %s '%s' is not a readable regular file\n", description, path);
        return -1;
    }
    return 0;
}

static char *read_text_file(const char *path)
{
    FILE *fp;
    struct stat st;
    char *buf;
    size_t got;
    if (!path) return NULL;
    if (validate_regular_file(path, "annotation file") < 0) return NULL;
    if (stat(path, &st) < 0 || st.st_size > 1024 * 1024) {
        errorf("annotation file is too large (maximum 1 MiB)"); return NULL;
    }
    fp = fopen(path, "rb");
    if (!fp) return NULL;
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(fp); return NULL; }
    got = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (got != (size_t)st.st_size) { free(buf); return NULL; }
    buf[got] = '\0';
    return buf;
}

static int run_ffmpeg(const struct options *opt, const char *output)
{
    pid_t pid;
    int status;
    time_t deadline;
    char *const argv[] = {(char *)opt->ffmpeg, "-nostdin", "-y", "-i",
        (char *)opt->input, "-map", "0:v:0", "-map", "0:a:0", "-c:v", "copy",
        "-c:a", "aac", "-b:a", "48k", "-movflags", "+faststart",
        (char *)output, NULL};
    int rc = posix_spawnp(&pid, opt->ffmpeg, NULL, NULL, argv, environ);
    if (rc) {
        fprintf(stderr, "ERROR: cannot start ffmpeg '%s': %s\n", opt->ffmpeg, strerror(rc));
        return -1;
    }
    deadline = time(NULL) + opt->timeout;
    for (;;) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0) { errorf("could not wait for ffmpeg"); return -1; }
        if (interrupted || time(NULL) >= deadline) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            errorf(interrupted ? "interrupted during ffmpeg conversion" : "ffmpeg conversion timed out");
            return -1;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status)) fprintf(stderr, "ERROR: ffmpeg exited with status %d\n", WEXITSTATUS(status));
        else errorf("ffmpeg terminated abnormally");
        return -1;
    }
    return 0;
}

static char *jid_domain(const char *jid)
{
    const char *at = strchr(jid, '@');
    const char *start = at ? at + 1 : jid;
    const char *slash = strchr(start, '/');
    size_t len = slash ? (size_t)(slash - start) : strlen(start);
    return len ? strndup(start, len) : NULL;
}

static char *sha256_base64(const char *path)
{
    unsigned char buffer[16384], digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX *md = NULL;
    FILE *fp = NULL;
    char *encoded = NULL;
    size_t count;

    fp = fopen(path, "rb");
    md = EVP_MD_CTX_new();
    if (!fp || !md || EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1) goto done;
    while ((count = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (EVP_DigestUpdate(md, buffer, count) != 1) goto done;
    }
    if (ferror(fp) || EVP_DigestFinal_ex(md, digest, &digest_len) != 1) goto done;
    encoded = malloc(4 * ((digest_len + 2) / 3) + 1);
    if (!encoded || EVP_EncodeBlock((unsigned char *)encoded, digest, (int)digest_len) < 0) {
        free(encoded); encoded = NULL;
    }
done:
    if (fp) fclose(fp);
    EVP_MD_CTX_free(md);
    return encoded;
}

static char *full_jid(const struct config *cfg)
{
    size_t n;
    char *jid;
    if (strchr(cfg->jid, '/')) return strdup(cfg->jid);
    n = strlen(cfg->jid) + strlen(cfg->resource) + 2;
    jid = malloc(n);
    if (jid) snprintf(jid, n, "%s/%s", cfg->jid, cfg->resource);
    return jid;
}

static char *new_id(struct app *app, const char *prefix)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s-%d", prefix, app->next_id++);
    return strdup(buf);
}

static void finish(struct app *app, int result, const char *message)
{
    if (app->finished) return;
    app->finished = true;
    app->result = result;
    if (message) errorf(message);
    if (app->conn && xmpp_conn_is_connected(app->conn)) xmpp_disconnect(app->conn);
}

static void send_iq_query(struct app *app, const char *to, const char *ns,
                          const char *prefix, xmpp_handler handler, void *userdata)
{
    char *id = new_id(app, prefix);
    xmpp_stanza_t *iq = xmpp_iq_new(app->ctx, "get", id);
    xmpp_stanza_t *query = xmpp_stanza_new(app->ctx);
    xmpp_stanza_set_name(query, "query"); xmpp_stanza_set_ns(query, ns);
    xmpp_stanza_set_to(iq, to); xmpp_stanza_add_child(iq, query);
    xmpp_id_handler_add(app->conn, handler, id, userdata);
    xmpp_send(app->conn, iq);
    xmpp_stanza_release(query); xmpp_stanza_release(iq); free(id);
}

static bool has_feature(xmpp_stanza_t *iq, const char *feature)
{
    xmpp_stanza_t *query = xmpp_stanza_get_child_by_name_and_ns(iq, "query", DISCO_INFO_NS);
    xmpp_stanza_t *child;
    for (child = query ? xmpp_stanza_get_children(query) : NULL; child;
         child = xmpp_stanza_get_next(child)) {
        const char *var;
        if (xmpp_stanza_is_tag(child) && !strcmp(xmpp_stanza_get_name(child), "feature") &&
            (var = xmpp_stanza_get_attribute(child, "var")) && !strcmp(var, feature)) return true;
    }
    return false;
}

static int slot_handler(xmpp_conn_t *, xmpp_stanza_t *, void *);

static void request_slot(struct app *app)
{
    char sizebuf[32];
    char *id = new_id(app, "slot");
    xmpp_stanza_t *iq = xmpp_iq_new(app->ctx, "get", id);
    xmpp_stanza_t *request = xmpp_stanza_new(app->ctx);
    snprintf(sizebuf, sizeof(sizebuf), "%lld", (long long)app->file_size);
    xmpp_stanza_set_name(request, "request"); xmpp_stanza_set_ns(request, UPLOAD_NS);
    xmpp_stanza_set_attribute(request, "filename", app->filename);
    xmpp_stanza_set_attribute(request, "size", sizebuf);
    xmpp_stanza_set_attribute(request, "content-type", "video/mp4");
    xmpp_stanza_set_to(iq, app->upload_service); xmpp_stanza_add_child(iq, request);
    xmpp_id_handler_add(app->conn, slot_handler, id, app);
    xmpp_send(app->conn, iq);
    xmpp_stanza_release(request); xmpp_stanza_release(iq); free(id);
    app->slot_requested = true;
    if (app->opt.debug) fprintf(stderr, "DEBUG: requesting upload slot from %s\n", app->upload_service);
}

static void maybe_discovery_failed(struct app *app)
{
    if (!app->slot_requested && app->items_done && app->info_pending == 0)
        finish(app, EXIT_DISCOVERY_ERROR,
            "XMPP server does not advertise HTTP File Upload (XEP-0363)");
}

static int info_handler(xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata)
{
    struct app *app = userdata;
    const char *type = xmpp_stanza_get_type(stanza);
    const char *from = xmpp_stanza_get_from(stanza);
    (void)conn;
    app->info_pending--;
    if (!app->slot_requested && type && !strcmp(type, "result") && has_feature(stanza, UPLOAD_NS)) {
        app->upload_service = strdup(from ? from : app->domain);
        if (!app->upload_service) finish(app, EXIT_DISCOVERY_ERROR, "out of memory");
        else request_slot(app);
    }
    maybe_discovery_failed(app);
    return 0;
}

static int items_handler(xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata)
{
    struct app *app = userdata;
    xmpp_stanza_t *query, *item;
    const char *type = xmpp_stanza_get_type(stanza);
    (void)conn;
    app->items_done = true;
    query = type && !strcmp(type, "result") ?
        xmpp_stanza_get_child_by_name_and_ns(stanza, "query", DISCO_ITEMS_NS) : NULL;
    for (item = query ? xmpp_stanza_get_children(query) : NULL; item;
         item = xmpp_stanza_get_next(item)) {
        const char *jid = xmpp_stanza_get_attribute(item, "jid");
        if (jid && *jid) {
            app->info_pending++;
            send_iq_query(app, jid, DISCO_INFO_NS, "info", info_handler, app);
        }
    }
    maybe_discovery_failed(app);
    return 0;
}

static int send_done_handler(xmpp_conn_t *conn, void *userdata)
{
    struct app *app = userdata;
    if (xmpp_conn_send_queue_len(conn) == 0) finish(app, 0, NULL);
    return app->finished ? 0 : 1;
}

static int http_put(struct app *app, const char *url, xmpp_stanza_t *put)
{
    CURL *curl = NULL;
    FILE *fp = NULL;
    CURLcode rc;
    long response = 0;
    struct curl_slist *headers = NULL;
    xmpp_stanza_t *node;

    if (strncmp(url, "https://", 8) != 0) { errorf("upload slot returned a non-HTTPS PUT URL"); return -1; }
    for (node = xmpp_stanza_get_children(put); node; node = xmpp_stanza_get_next(node)) {
        const char *name;
        char *value;
        char line[4096];
        if (!xmpp_stanza_is_tag(node) || strcmp(xmpp_stanza_get_name(node), "header")) continue;
        name = xmpp_stanza_get_attribute(node, "name");
        value = xmpp_stanza_get_text(node);
        if (!name || !value || strchr(name, '\r') || strchr(name, '\n') ||
            strchr(value, '\r') || strchr(value, '\n') ||
            strlen(name) + strlen(value) + 3 > sizeof(line)) {
            if (value) xmpp_free(app->ctx, value);
            errorf("upload slot returned an invalid HTTP header");
            goto fail;
        }
        strcpy(line, name);
        strcat(line, ": ");
        strcat(line, value);
        headers = curl_slist_append(headers, line); xmpp_free(app->ctx, value);
        if (!headers) goto fail;
    }
    headers = curl_slist_append(headers, "Content-Type: video/mp4");
    fp = fopen(app->mp4, "rb"); curl = curl_easy_init();
    if (!fp || !curl || !headers) goto fail;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, fp);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)app->file_size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, app->opt.timeout);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "visualptt-xmpp-send/1");
    if (app->opt.debug) fprintf(stderr, "DEBUG: uploading %lld bytes with HTTPS PUT\n",
                                (long long)app->file_size);
    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    curl_easy_cleanup(curl); fclose(fp); curl_slist_free_all(headers);
    if (rc != CURLE_OK) { fprintf(stderr, "ERROR: HTTP PUT failed: %s\n", curl_easy_strerror(rc)); return -1; }
    if (response < 200 || response >= 300) { fprintf(stderr, "ERROR: HTTP PUT returned status %ld\n", response); return -1; }
    return 0;
fail:
    if (curl) curl_easy_cleanup(curl);
    if (fp) fclose(fp);
    curl_slist_free_all(headers);
    errorf("could not prepare HTTP PUT"); return -1;
}

static xmpp_stanza_t *text_element(xmpp_ctx_t *ctx, const char *name,
                                   const char *ns, const char *text)
{
    xmpp_stanza_t *element = xmpp_stanza_new(ctx);
    xmpp_stanza_t *text_node = xmpp_stanza_new(ctx);
    if (!element || !text_node) {
        if (element) xmpp_stanza_release(element);
        if (text_node) xmpp_stanza_release(text_node);
        return NULL;
    }
    if (xmpp_stanza_set_name(element, name) < 0 ||
        (ns && xmpp_stanza_set_ns(element, ns) < 0) ||
        xmpp_stanza_set_text(text_node, text) < 0 ||
        xmpp_stanza_add_child(element, text_node) < 0) {
        xmpp_stanza_release(element);
        xmpp_stanza_release(text_node);
        return NULL;
    }
    xmpp_stanza_release(text_node);
    return element;
}

static void send_message(struct app *app, const char *url)
{
    size_t len = strlen(app->body_prefix) + strlen(url) + 3;
    char *body = malloc(len);
    char sizebuf[32];
    char *message_id = new_id(app, "media");
    xmpp_stanza_t *msg = NULL, *x = NULL, *url_node = NULL;
    xmpp_stanza_t *sharing = NULL, *file = NULL, *media_type = NULL, *media_type_dino = NULL;
    xmpp_stanza_t *name = NULL, *size = NULL, *hash = NULL;
    xmpp_stanza_t *sources = NULL, *url_data = NULL, *dino_sources = NULL, *dino_url = NULL;
    if (!body || !message_id) {
        free(body); free(message_id);
        finish(app, EXIT_MESSAGE_ERROR, "could not construct XMPP message");
        return;
    }
    snprintf(body, len, "%s\n\n%s", app->body_prefix, url);
    snprintf(sizebuf, sizeof(sizebuf), "%lld", (long long)app->file_size);
    msg = xmpp_message_new(app->ctx, "normal", app->opt.to, message_id);
    x = xmpp_stanza_new(app->ctx);
    url_node = text_element(app->ctx, "url", NULL, url);
    sharing = xmpp_stanza_new(app->ctx); file = xmpp_stanza_new(app->ctx);
    media_type = text_element(app->ctx, "media-type", NULL, "video/mp4");
    /* Dino 0.5 parses this older underscore spelling. */
    media_type_dino = text_element(app->ctx, "media_type", NULL, "video/mp4");
    name = text_element(app->ctx, "name", NULL, app->filename);
    size = text_element(app->ctx, "size", NULL, sizebuf);
    hash = text_element(app->ctx, "hash", HASHES_NS, app->sha256);
    sources = xmpp_stanza_new(app->ctx); url_data = xmpp_stanza_new(app->ctx);
    dino_sources = xmpp_stanza_new(app->ctx); dino_url = xmpp_stanza_new(app->ctx);
    if (!msg || !x || !url_node || !sharing || !file || !media_type ||
        !media_type_dino || !name || !size || !hash || !sources || !url_data ||
        !dino_sources || !dino_url || xmpp_message_set_body(msg, body) < 0)
        goto stanza_fail;

    /* XEP-0447 tells modern clients this is inline video. XEP-0066 and the
       body remain as compatibility fallbacks for clients without SFS. */
    if (xmpp_stanza_set_name(sharing, "file-sharing") < 0 ||
        xmpp_stanza_set_ns(sharing, SFS_NS) < 0 ||
        xmpp_stanza_set_attribute(sharing, "disposition", "inline") < 0 ||
        xmpp_stanza_set_name(file, "file") < 0 ||
        xmpp_stanza_set_ns(file, FILE_METADATA_NS) < 0 ||
        xmpp_stanza_set_name(sources, "sources") < 0 ||
        xmpp_stanza_set_ns(sources, SFS_NS) < 0 ||
        xmpp_stanza_set_name(url_data, "url-data") < 0 ||
        xmpp_stanza_set_ns(url_data, URL_DATA_NS) < 0 ||
        xmpp_stanza_set_attribute(url_data, "target", url) < 0 ||
        xmpp_stanza_set_attribute(hash, "algo", "sha-256") < 0 ||
        xmpp_stanza_set_name(dino_sources, "sources") < 0 ||
        xmpp_stanza_set_ns(dino_sources, SFS_NS) < 0 ||
        xmpp_stanza_set_name(dino_url, "url-data") < 0 ||
        xmpp_stanza_set_ns(dino_url, URL_DATA_NS) < 0 ||
        xmpp_stanza_set_attribute(dino_url, "target", url) < 0)
        goto stanza_fail;
    xmpp_stanza_add_child(file, media_type);
    xmpp_stanza_add_child(file, media_type_dino);
    xmpp_stanza_add_child(file, name);
    xmpp_stanza_add_child(file, size);
    xmpp_stanza_add_child(file, hash);
    xmpp_stanza_add_child(sources, url_data);
    xmpp_stanza_add_child(dino_sources, dino_url);
    xmpp_stanza_add_child(sharing, file);
    xmpp_stanza_add_child(sharing, sources);
    xmpp_stanza_add_child(msg, sharing);
    /* Dino 0.5 looks for sources on the message root. Keep the standards-based
       nested source above and add this compatibility copy for that release. */
    xmpp_stanza_add_child(msg, dino_sources);

    xmpp_stanza_set_name(x, "x"); xmpp_stanza_set_ns(x, OOB_NS);
    xmpp_stanza_add_child(x, url_node); xmpp_stanza_add_child(msg, x);
    xmpp_send(app->conn, msg);
    xmpp_stanza_release(dino_url); xmpp_stanza_release(dino_sources);
    xmpp_stanza_release(url_data); xmpp_stanza_release(sources); xmpp_stanza_release(hash);
    xmpp_stanza_release(size); xmpp_stanza_release(name);
    xmpp_stanza_release(media_type_dino); xmpp_stanza_release(media_type);
    xmpp_stanza_release(file); xmpp_stanza_release(sharing);
    xmpp_stanza_release(url_node); xmpp_stanza_release(x); xmpp_stanza_release(msg);
    free(message_id); free(body);
    xmpp_timed_handler_add(app->conn, send_done_handler, 100, app);
    if (app->opt.debug) fprintf(stderr, "DEBUG: attachment message queued for %s\n", app->opt.to);
    return;

stanza_fail:
    if (dino_url) xmpp_stanza_release(dino_url);
    if (dino_sources) xmpp_stanza_release(dino_sources);
    if (url_data) xmpp_stanza_release(url_data);
    if (sources) xmpp_stanza_release(sources);
    if (hash) xmpp_stanza_release(hash);
    if (size) xmpp_stanza_release(size);
    if (name) xmpp_stanza_release(name);
    if (media_type_dino) xmpp_stanza_release(media_type_dino);
    if (media_type) xmpp_stanza_release(media_type);
    if (file) xmpp_stanza_release(file);
    if (sharing) xmpp_stanza_release(sharing);
    if (url_node) xmpp_stanza_release(url_node);
    if (x) xmpp_stanza_release(x);
    if (msg) xmpp_stanza_release(msg);
    free(message_id); free(body);
    finish(app, EXIT_MESSAGE_ERROR, "could not construct XMPP message");
}

static int slot_handler(xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata)
{
    struct app *app = userdata;
    xmpp_stanza_t *slot, *put, *get;
    const char *put_url, *get_url;
    (void)conn;
    if (!xmpp_stanza_get_type(stanza) || strcmp(xmpp_stanza_get_type(stanza), "result")) {
        finish(app, EXIT_SLOT_ERROR, "HTTP upload slot request failed"); return 0;
    }
    slot = xmpp_stanza_get_child_by_name_and_ns(stanza, "slot", UPLOAD_NS);
    put = slot ? xmpp_stanza_get_child_by_name(slot, "put") : NULL;
    get = slot ? xmpp_stanza_get_child_by_name(slot, "get") : NULL;
    put_url = put ? xmpp_stanza_get_attribute(put, "url") : NULL;
    get_url = get ? xmpp_stanza_get_attribute(get, "url") : NULL;
    if (!put_url || !get_url || strncmp(get_url, "https://", 8)) {
        finish(app, EXIT_SLOT_ERROR, "upload slot response contains invalid or non-HTTPS URLs"); return 0;
    }
    if (http_put(app, put_url, put) < 0) { finish(app, EXIT_HTTP_ERROR, NULL); return 0; }
    send_message(app, get_url);
    return 0;
}

static void connection_handler(xmpp_conn_t *conn, xmpp_conn_event_t event, int error,
                               xmpp_stream_error_t *stream_error, void *userdata)
{
    struct app *app = userdata;
    (void)error; (void)stream_error;
    if (event == XMPP_CONN_RAW_CONNECT) {
        app->raw_connected = true;
        if (app->opt.debug) fprintf(stderr, "DEBUG: connected to XMPP server; negotiating TLS and SASL\n");
    } else if (event == XMPP_CONN_CONNECT) {
        app->connected_once = true;
        if (!xmpp_conn_is_secured(conn)) {
            finish(app, EXIT_CONNECT_ERROR, "XMPP connection is not protected by TLS"); return;
        }
        if (app->opt.debug) fprintf(stderr, "DEBUG: authenticated as %s using TLS\n", xmpp_conn_get_bound_jid(conn));
        app->info_pending = 1;
        send_iq_query(app, app->domain, DISCO_INFO_NS, "info", info_handler, app);
        send_iq_query(app, app->domain, DISCO_ITEMS_NS, "items", items_handler, app);
    } else if (!app->finished) {
        if (app->connected_once)
            finish(app, EXIT_MESSAGE_ERROR, "XMPP connection closed before completion");
        else if (!app->raw_connected)
            finish(app, EXIT_CONNECT_ERROR, "XMPP DNS or connection failed");
        else if (stream_error && stream_error->type == XMPP_SE_NOT_AUTHORIZED)
            finish(app, EXIT_AUTH_ERROR, "XMPP authentication failed");
        else
            finish(app, EXIT_AUTH_ERROR, "XMPP TLS or authentication failed");
    }
}

static int run_xmpp(struct app *app)
{
    char *jid = full_jid(&app->cfg);
    /* libstrophe's raw debug logger can expose SASL payloads.  Use explicit,
       secret-free diagnostics above instead. */
    xmpp_log_t *logger = NULL;
    if (!jid) { errorf("out of memory"); return EXIT_CONNECT_ERROR; }
    xmpp_initialize();
    app->ctx = xmpp_ctx_new(NULL, logger);
    app->conn = app->ctx ? xmpp_conn_new(app->ctx) : NULL;
    if (!app->conn) { free(jid); errorf("could not initialize XMPP client"); return EXIT_CONNECT_ERROR; }
    xmpp_conn_set_jid(app->conn, jid); xmpp_conn_set_pass(app->conn, app->cfg.password);
    xmpp_conn_set_flags(app->conn, XMPP_CONN_FLAG_MANDATORY_TLS | XMPP_CONN_FLAG_DISABLE_SM);
    app->deadline = time(NULL) + app->opt.timeout;
    if (xmpp_connect_client(app->conn, app->cfg.server, app->cfg.port,
                            connection_handler, app) != 0) {
        errorf("could not start XMPP connection"); app->result = EXIT_CONNECT_ERROR; app->finished = true;
    }
    while (!app->finished && !interrupted && time(NULL) < app->deadline)
        xmpp_run_once(app->ctx, 100);
    if (!app->finished) finish(app, interrupted ? 128 + SIGINT : EXIT_CONNECT_ERROR,
        interrupted ? "interrupted" : "XMPP operation timed out");
    xmpp_run_once(app->ctx, 20);
    xmpp_conn_release(app->conn); xmpp_ctx_free(app->ctx); xmpp_shutdown(); free(jid);
    app->conn = NULL; app->ctx = NULL;
    return app->result;
}

int main(int argc, char **argv)
{
    struct app app;
    struct stat st;
    char temp_template[] = "/tmp/visualptt-xmpp-XXXXXX";
    char *temp_dir = NULL, *mp4 = NULL, *config_path = NULL, *annotation = NULL;
    size_t n;
    int parsed, result = EXIT_CONFIG_ERROR;
    bool converted = false;

    memset(&app, 0, sizeof(app));
    parsed = parse_options(argc, argv, &app.opt);
    if (parsed > 0) return 0;
    if (parsed < 0) return EXIT_USAGE_ERROR;
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
    if (validate_regular_file(app.opt.input, "input file") < 0) return EXIT_INPUT_ERROR;
    n = strlen(app.opt.input);
    if (n < 4 || strcasecmp(app.opt.input + n - 4, ".mkv")) {
        errorf("input file must have an .mkv extension"); return EXIT_INPUT_ERROR;
    }
    if (app.opt.annotation) {
        annotation = read_text_file(app.opt.annotation);
        if (!annotation) return EXIT_INPUT_ERROR;
    }
    config_path = app.opt.config ? strdup(app.opt.config) : default_config_path();
    if (!config_path) { errorf("HOME is not set; use --config FILE"); goto cleanup; }
    if (load_config(config_path, &app.cfg) < 0) goto cleanup;
    temp_dir = mkdtemp(temp_template);
    if (!temp_dir) { errorf("could not create temporary directory"); result = EXIT_CONVERT_ERROR; goto cleanup; }
    n = strlen(temp_dir) + strlen("/output.mp4") + 1;
    mp4 = malloc(n);
    if (!mp4) { errorf("out of memory"); result = EXIT_CONVERT_ERROR; goto cleanup; }
    snprintf(mp4, n, "%s/output.mp4", temp_dir);
    if (run_ffmpeg(&app.opt, mp4) < 0) { result = EXIT_CONVERT_ERROR; goto cleanup; }
    if (stat(mp4, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        errorf("ffmpeg did not create a valid MP4 file"); result = EXIT_CONVERT_ERROR; goto cleanup;
    }
    converted = true;
    app.mp4 = mp4; app.file_size = st.st_size; app.filename = "visualptt-message.mp4";
    app.sha256 = sha256_base64(mp4);
    app.domain = jid_domain(app.cfg.jid);
    n = strlen(app.opt.message) + (annotation ? strlen(annotation) + 3 : 1);
    app.body_prefix = malloc(n);
    if (!app.sha256) { errorf("could not compute MP4 SHA-256 hash"); result = EXIT_CONVERT_ERROR; goto cleanup; }
    if (!app.domain || !app.body_prefix) { errorf("out of memory"); result = EXIT_CONNECT_ERROR; goto cleanup; }
    snprintf(app.body_prefix, n, annotation && *annotation ? "%s\n\n%s" : "%s%s",
             app.opt.message, annotation && *annotation ? annotation : "");
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        errorf("could not initialize HTTP client"); result = EXIT_HTTP_ERROR; goto cleanup;
    }
    result = run_xmpp(&app);
    curl_global_cleanup();
cleanup:
    if (app.opt.keep && converted && mp4 && access(mp4, F_OK) == 0) {
        printf("Converted MP4 retained at: %s\n", mp4);
    } else {
        if (mp4) unlink(mp4);
        if (temp_dir) rmdir(temp_dir);
    }
    free(app.domain); free(app.body_prefix); free(app.sha256); free(app.upload_service);
    free(annotation); free(config_path); free(mp4); config_free(&app.cfg);
    return result;
}
