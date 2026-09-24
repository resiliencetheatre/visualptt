/* Native collector regression and crash tests; no interpreter needed. */
#define COLLECTOR_TESTING
#define main collector_program_main
#include "../visualptt-collector.c"
#undef main
#include <assert.h>
#include <ftw.h>
#include <sys/wait.h>

#define NEW "rec_20260923_091500_abababababababababababababababab.mkv"
#define LEGACY "rec_20260923_091500.mkv"
#define SECOND "rec_20260923_091501.mkv"

typedef struct {
    char root[128], config[160];
    Collector collector;
    bool opened;
} Fixture;
static Fixture *active;
static const char *kill_stage;
static int hook_mode, short_writes;
static unsigned int checks;

ssize_t __real_write(int fd, const void *buf, size_t size);
ssize_t __wrap_write(int fd, const void *buf, size_t size)
{
    if (short_writes && size > 37) size = 37;
    return __real_write(fd, buf, size);
}

static void path_for(Fixture *f, const char *relative, char out[PATH_MAX])
{
    assert(snprintf(out, PATH_MAX, "%s/%s", f->root, relative) < PATH_MAX);
}

static void put(Fixture *f, const char *relative, const char *data)
{
    char path[PATH_MAX]; path_for(f, relative, path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0 && write_all(fd, data, strlen(data)) == 0 && close(fd) == 0);
}

static bool exists(Fixture *f, const char *relative)
{
    char path[PATH_MAX]; struct stat st; path_for(f, relative, path);
    return lstat(path, &st) == 0;
}

static void erase(Fixture *f, const char *relative)
{
    char path[PATH_MAX]; path_for(f, relative, path);
    assert(unlink(path) == 0);
}

static void contents(Fixture *f, const char *relative, const char *expected)
{
    char path[PATH_MAX], data[8192]; path_for(f, relative, path);
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0);
    ssize_t size = read(fd, data, sizeof(data));
    assert(size == (ssize_t)strlen(expected) && !memcmp(data, expected, (size_t)size));
    assert(close(fd) == 0);
}

static int count_entries(Fixture *f, const char *relative)
{
    char path[PATH_MAX]; path_for(f, relative, path);
    DIR *dir = opendir(path); assert(dir);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) count++;
    closedir(dir); return count;
}

static void configuration(Fixture *f, const char *extra, const char *sources)
{
    FILE *file = fopen(f->config, "w"); assert(file);
    fprintf(file, "[collector]\nlocal_peer=C\ninput_dir=%s/input\nstate_dir=%s/state\n%s\n[sources]\n",
            f->root, f->root, extra ? extra : "");
    if (sources) fputs(sources, file);
    else fprintf(file, "A=%s/A\nB=%s/B\n", f->root, f->root);
    assert(fclose(file) == 0);
}

static void fixture(Fixture *f)
{
    memset(f, 0, sizeof(*f));
    strcpy(f->root, "/tmp/visualptt-native-XXXXXX");
    assert(mkdtemp(f->root));
    const char *dirs[] = {"input", "state", "own", "A", "B"};
    for (size_t i = 0; i < sizeof dirs/sizeof *dirs; i++) {
        char path[PATH_MAX]; path_for(f, dirs[i], path);
        assert(mkdir(path, 0700) == 0);
    }
    snprintf(f->config, sizeof f->config, "%s/collector.ini", f->root);
    configuration(f, NULL, NULL);
    active = f;
}

static void start(Fixture *f)
{
    assert(!f->opened && collector_open(&f->collector, f->config) == 0);
    f->opened = true;
}

static void stop(Fixture *f)
{
    if (f->opened) collector_close(&f->collector);
    f->opened = false;
}

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *info)
{
    (void)st; (void)type; (void)info;
    return remove(path);
}

static void cleanup(Fixture *f)
{
    stop(f); test_hook = NULL; short_writes = 0;
    assert(nftw(f->root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    checks++;
}

static int query_count(Collector *c, const char *sql)
{
    sqlite3_stmt *stmt = sql_prepare(c, sql, 0);
    assert(sql_step(c, stmt) == SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt); return count;
}

static void assert_no_hash(const char *stage) { assert(strcmp(stage, "verify") && strcmp(stage, "copy")); }

static void test_delivery_restart(void)
{
    Fixture f; fixture(&f);
    put(&f, "A/" NEW, "recording"); put(&f, "B/" NEW, "from B");
    put(&f, "A/" LEGACY, "old A"); put(&f, "B/" LEGACY, "old B");
    start(&f); assert(scan_sources(&f.collector) == 0);
    assert(count_entries(&f, "input") == 4);
    contents(&f, "input/B_" LEGACY, "old B");
    erase(&f, "input/A_" NEW); erase(&f, "input/B_" NEW);
    erase(&f, "input/A_" LEGACY); erase(&f, "input/B_" LEGACY);
    stop(&f); start(&f);
    test_hook = assert_no_hash;
    assert(scan_sources(&f.collector) == 0 && count_entries(&f, "input") == 0);
    assert(exists(&f, "A/" NEW));
    test_hook = NULL;
    /* Reintroduced source identity remains suppressed. */
    erase(&f, "A/" NEW); put(&f, "A/" NEW, "changed bytes, old identity");
    assert(scan_sources(&f.collector) == 0 && count_entries(&f, "input") == 0);
    /* Polling is a fresh directory traversal and finds new files later. */
    put(&f, "A/" SECOND, "later");
    assert(scan_sources(&f.collector) == 0); contents(&f, "input/A_" SECOND, "later");
    cleanup(&f);
}

static void test_safe_sources(void)
{
    Fixture f; fixture(&f); configuration(&f, "stable_scans=2", NULL);
    put(&f, "A/" NEW, "recording"); put(&f, "A/.syncthing.tmp", "temporary");
    put(&f, "A/" NEW ".bad", "bad suffix"); put(&f, "A/" SECOND, "");
    char path[PATH_MAX]; path_for(&f, "A/" LEGACY, path); assert(symlink("/etc/passwd", path) == 0);
    path_for(&f, "A/rec_20260923_091502.mkv", path); assert(mkfifo(path, 0600) == 0);
    path_for(&f, "A/rec_20260923_091503.mkv", path); assert(mkdir(path, 0700) == 0);
    put(&f, "B/rec_20260923_091504.mkv", "hardlinked");
    char target[PATH_MAX]; path_for(&f, "B/rec_20260923_091504.mkv", target);
    path_for(&f, "A/rec_20260923_091504.mkv", path); assert(link(target, path) == 0);
    start(&f);
    assert(scan_sources(&f.collector) == 0 && count_entries(&f, "input") == 0);
    assert(scan_sources(&f.collector) == 0 && count_entries(&f, "input") == 1);
    contents(&f, "input/A_" NEW, "recording");
    cleanup(&f);
}

static void mutate_hook(const char *stage)
{
    if (strcmp(stage, "copy")) return;
    assert(!exists(active, "input/A_" NEW));
    test_hook = NULL;
    if (hook_mode == 2) erase(active, "A/" NEW);
    put(active, "A/" NEW, hook_mode == 2 ? "replacement" : "modified source");
}

static void test_source_change(int mode)
{
    Fixture f; fixture(&f); put(&f, "A/" NEW, "original"); start(&f);
    hook_mode = mode; test_hook = mutate_hook;
    assert(scan_sources(&f.collector) == 0 && count_entries(&f, "input") == 0);
    assert(query_count(&f.collector, "SELECT count(*) FROM delivery") == 0);
    assert(scan_sources(&f.collector) == 0);
    contents(&f, "input/A_" NEW, mode == 2 ? "replacement" : "modified source");
    cleanup(&f);
}

static void invisible_hook(const char *stage)
{
    if (!strcmp(stage, "copy")) assert(!exists(active, "input/A_" NEW));
}

static void test_short_copy(void)
{
    Fixture f; fixture(&f);
    char data[4000]; memset(data, 'x', sizeof data - 1); data[sizeof data - 1] = 0;
    put(&f, "A/" NEW, data); start(&f);
    test_hook = invisible_hook; short_writes = 1;
    assert(scan_sources(&f.collector) == 0);
    contents(&f, "input/A_" NEW, data);
    cleanup(&f);
}

static void test_collision(void)
{
    Fixture f; fixture(&f); put(&f, "A/" NEW, "recording"); put(&f, "input/A_" NEW, "different");
    start(&f); assert(scan_sources(&f.collector) == 0);
    contents(&f, "input/A_" NEW, "different");
    assert(query_count(&f.collector, "SELECT count(*) FROM delivery WHERE temp IS NOT NULL") == 1);
    stop(&f); start(&f); assert(scan_sources(&f.collector) == 0);
    contents(&f, "input/A_" NEW, "different");
    erase(&f, "input/A_" NEW); assert(scan_sources(&f.collector) == 0);
    contents(&f, "input/A_" NEW, "recording");
    put(&f, "B/" NEW, "same"); put(&f, "input/B_" NEW, "same");
    assert(scan_sources(&f.collector) == 0);
    assert(query_count(&f.collector, "SELECT count(*) FROM delivery WHERE temp IS NULL") == 2);
    cleanup(&f);
}

static void test_destination_symlink(void)
{
    Fixture f; fixture(&f); put(&f, "A/" NEW, "recording"); put(&f, "protected", "protected");
    char path[PATH_MAX], target[PATH_MAX];
    path_for(&f, "input/A_" NEW, path); path_for(&f, "protected", target);
    assert(symlink(target, path) == 0);
    start(&f); assert(scan_sources(&f.collector) == 0);
    contents(&f, "protected", "protected");
    struct stat st; assert(lstat(path, &st) == 0 && S_ISLNK(st.st_mode));
    cleanup(&f);
}

static void crash_hook(const char *stage)
{
    if (!strcmp(kill_stage, "consumed") && !strcmp(stage, "rename")) {
        erase(active, "input/A_" NEW);
        kill(getpid(), SIGKILL);
    }
    if (!strcmp(stage, kill_stage)) kill(getpid(), SIGKILL);
}

static void test_crash(const char *stage)
{
    Fixture f; fixture(&f); put(&f, "A/" NEW, "recording");
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        start(&f); kill_stage = stage; test_hook = crash_hook;
        scan_sources(&f.collector); _exit(99);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    start(&f); assert(scan_sources(&f.collector) == 0);
    if (strcmp(stage, "consumed")) {
        contents(&f, "input/A_" NEW, "recording"); erase(&f, "input/A_" NEW);
    }
    assert(scan_sources(&f.collector) == 0 && count_entries(&f, "input") == 0);
    assert(query_count(&f.collector, "SELECT count(*) FROM delivery WHERE temp IS NULL") == 1);
    cleanup(&f);
}

static void expect_bad(Fixture *f)
{
    Collector other;
    assert(collector_open(&other, f->config) != 0);
}

static void test_validation(void)
{
    Fixture f; fixture(&f); start(&f); expect_bad(&f); stop(&f);
    const char *bad[] = {"poll_seconds=nan", "retention_seconds=-1", "stable_scans=3",
        "local_peer=duplicate", "unknown_setting=true", "purge_enabled=true", "poll_seconds=2junk",
        "[collector]", "[unexpected]", "[DEFAULT]"};
    for (size_t i = 0; i < sizeof bad/sizeof *bad; i++) { configuration(&f, bad[i], NULL); expect_bad(&f); }
    char sources[1024];
    snprintf(sources, sizeof sources, "C=%s/A\n", f.root); configuration(&f, NULL, sources); expect_bad(&f);
    snprintf(sources, sizeof sources, "../A=%s/A\n", f.root); configuration(&f, NULL, sources); expect_bad(&f);
    snprintf(sources, sizeof sources, "A=%s/A\nA=%s/B\n", f.root, f.root); configuration(&f, NULL, sources); expect_bad(&f);
    snprintf(sources, sizeof sources, "A=%s/input\n", f.root); configuration(&f, NULL, sources); expect_bad(&f);
    snprintf(sources, sizeof sources, "A=%s\n", f.root); configuration(&f, NULL, sources); expect_bad(&f);
    char path[PATH_MAX]; path_for(&f, "input/child", path); assert(mkdir(path, 0700) == 0);
    snprintf(sources, sizeof sources, "A=%s/input/child\n", f.root); configuration(&f, NULL, sources); expect_bad(&f);
    path_for(&f, "link", path); assert(symlink("A", path) == 0);
    snprintf(sources, sizeof sources, "A=%s/link\n", f.root); configuration(&f, NULL, sources); expect_bad(&f);
    snprintf(sources, sizeof sources, "A=%s/A/../B\n", f.root); configuration(&f, NULL, sources); expect_bad(&f);
    configuration(&f, NULL, NULL);
    path_for(&f, "input", path); assert(chmod(path, 0777) == 0); expect_bad(&f); assert(chmod(path, 0700) == 0);
    cleanup(&f);
}

static void purge_configuration(Fixture *f)
{
    char extra[512]; snprintf(extra, sizeof extra, "own_pool=%s/own\npurge_enabled=true\nretention_seconds=100", f->root);
    configuration(f, extra, NULL);
}

static void test_purge(void)
{
    Fixture f; fixture(&f); purge_configuration(&f);
    put(&f, "own/" NEW, "old mtime"); put(&f, "A/" NEW, "remote");
    char path[PATH_MAX]; path_for(&f, "own/" NEW, path);
    const struct timespec old[2] = {{1,0}, {1,0}}; assert(utimensat(AT_FDCWD, path, old, 0) == 0);
    start(&f);
    assert(purge_pool(&f.collector, 1000) == 0 && purge_pool(&f.collector, 1099) == 0);
    assert(exists(&f, "own/" NEW));
    put(&f, "own/" NEW, "changed"); assert(purge_pool(&f.collector, 1101) == 0);
    stop(&f); start(&f);
    int locked = open(path, O_RDONLY); assert(locked >= 0 && flock(locked, LOCK_EX | LOCK_NB) == 0);
    assert(purge_pool(&f.collector, 1202) != 0 && exists(&f, "own/" NEW)); close(locked);
    assert(purge_pool(&f.collector, 1202) == 0 && purge_pool(&f.collector, 1202) == 0);
    assert(!exists(&f, "own/" NEW) && exists(&f, "A/" NEW));
    put(&f, "own/" LEGACY, "after clock reset"); assert(purge_pool(&f.collector, 1500) == 0);
    assert(purge_pool(&f.collector, 1400) == 0 && purge_pool(&f.collector, 1499) == 0);
    assert(exists(&f, "own/" LEGACY));
    put(&f, "own/.active.part", "recording");
    assert(purge_pool(&f.collector, 1501) == 0 && exists(&f, "own/.active.part"));
    cleanup(&f);
}

static void test_offline_history(void)
{
    Fixture b, c, origin; fixture(&b); fixture(&c); fixture(&origin);
    purge_configuration(&origin); put(&origin, "own/" NEW, "recording"); start(&origin);
    assert(purge_pool(&origin.collector, 1000) == 0);
    /* B synchronizes and consumes; C remains disconnected with an empty replica. */
    put(&b, "A/" NEW, "recording"); start(&b); assert(scan_sources(&b.collector) == 0);
    erase(&b, "input/A_" NEW); start(&c); assert(scan_sources(&c.collector) == 0);
    assert(count_entries(&c, "input") == 0 && exists(&b, "A/" NEW));
    assert(purge_pool(&origin.collector, 1099) == 0 && exists(&origin, "own/" NEW));
    put(&c, "A/" NEW, "recording"); assert(scan_sources(&c.collector) == 0);
    contents(&c, "input/A_" NEW, "recording");
    /* Next message expires while C is disconnected. Model propagated deletions. */
    put(&origin, "own/" SECOND, "expires"); assert(purge_pool(&origin.collector, 1100) == 0);
    put(&b, "A/" SECOND, "expires"); assert(scan_sources(&b.collector) == 0);
    assert(purge_pool(&origin.collector, 1201) == 0 && !exists(&origin, "own/" SECOND));
    erase(&b, "A/" NEW); erase(&b, "A/" SECOND); erase(&c, "A/" NEW);
    assert(scan_sources(&c.collector) == 0 && !exists(&c, "input/A_" SECOND));
    assert(exists(&b, "input/A_" SECOND) && exists(&c, "input/A_" NEW));
    cleanup(&b); cleanup(&c); cleanup(&origin);
}

static void test_upgrade_ledger(void)
{
    Fixture f; fixture(&f);
    /* Seed the old implementation's schema, binding and completed identity;
     * use literal fixture formatting rather than the native binding writer. */
    char path[PATH_MAX]; path_for(&f, "state/ledger.sqlite3", path);
    sqlite3 *db; assert(sqlite3_open(path, &db) == SQLITE_OK);
    char *sql = sqlite3_mprintf("CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "CREATE TABLE delivery(origin TEXT NOT NULL,name TEXT NOT NULL,temp TEXT,size INTEGER NOT NULL,"
        "digest TEXT NOT NULL,PRIMARY KEY(origin,name));"
        "INSERT INTO metadata VALUES('binding',%Q);", "placeholder");
    assert(sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK); sqlite3_free(sql);
    char expected[1024]; snprintf(expected, sizeof expected, "('C', [('input', '%s/input'), ('state', '%s/state')])", f.root, f.root);
    sql = sqlite3_mprintf("UPDATE metadata SET value=%Q; INSERT INTO delivery VALUES('A',%Q,NULL,9,%Q);",
                         expected, NEW, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    assert(sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK); sqlite3_free(sql); sqlite3_close(db);
    put(&f, "A/" NEW, "recording"); start(&f);
    assert(scan_sources(&f.collector) == 0 && count_entries(&f, "input") == 0);
    stop(&f);
    /* Deployment mismatch must fail, never reset the historical ledger. */
    FILE *file = fopen(f.config, "w"); assert(file);
    fprintf(file, "[collector]\nlocal_peer=D\ninput_dir=%s/input\nstate_dir=%s/state\n[sources]\nA=%s/A\n", f.root, f.root, f.root);
    fclose(file); expect_bad(&f);
    cleanup(&f);
}

static void test_sql_failure(void)
{
    Fixture f; fixture(&f); put(&f, "A/" NEW, "recording"); start(&f);
    assert(sql_exec(&f.collector, "CREATE TRIGGER reject_intent BEFORE INSERT ON delivery BEGIN SELECT RAISE(FAIL,'injected'); END;") == 0);
    assert(scan_sources(&f.collector) != 0 && f.collector.failed);
    assert(!exists(&f, "input/A_" NEW) && count_entries(&f, "input") == 1);
    /* A failed/uncommitted intent leaves an orphan, not a falsely completed row. */
    assert(sql_exec(&f.collector, "DROP TRIGGER reject_intent") == 0);
    stop(&f); start(&f); assert(scan_sources(&f.collector) == 0);
    contents(&f, "input/A_" NEW, "recording");
    cleanup(&f);
}

static void test_cli(void)
{
    Fixture f; fixture(&f); put(&f, "A/" NEW, "recording");
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        /* Empty PATH proves this executable does not launch an interpreter. */
        setenv("PATH", "/nonexistent", 1);
        execl("./visualptt-collector", "visualptt-collector", "--config", f.config, "--once", (char *)NULL);
        _exit(99);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    contents(&f, "input/A_" NEW, "recording"); cleanup(&f);
}

static void test_daemon(void)
{
    Fixture f; fixture(&f); configuration(&f, "poll_seconds=0.02\nstable_scans=2", NULL);
    put(&f, "A/" NEW, "recording");
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        execl("./visualptt-collector", "visualptt-collector", "--config", f.config, (char *)NULL);
        _exit(99);
    }
    double limit = monotonic_seconds() + 5;
    while (!exists(&f, "input/A_" NEW) && monotonic_seconds() < limit) usleep(10000);
    bool delivered = exists(&f, "input/A_" NEW);
    assert(kill(child, SIGTERM) == 0);
    int status;
    while (waitpid(child, &status, WNOHANG) == 0 && monotonic_seconds() < limit) usleep(10000);
    if (monotonic_seconds() >= limit) { kill(child, SIGKILL); waitpid(child, &status, 0); assert(!"daemon timed out"); }
    assert(delivered && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    cleanup(&f);
}

static void assert_digest(VisualPttSha256 *ctx, const char *expected)
{
    unsigned char digest[32]; char hex[65];
    visualptt_sha256_final(ctx, digest);
    for (unsigned i = 0; i < sizeof digest; i++) sprintf(hex + 2*i, "%02x", digest[i]);
    assert(!strcmp(hex, expected));
}

static void test_sha256(void)
{
    const struct { const char *input, *digest; } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}
    };
    VisualPttSha256 ctx;
    for (size_t i = 0; i < sizeof vectors/sizeof *vectors; i++) {
        visualptt_sha256_init(&ctx);
        assert(visualptt_sha256_update(&ctx, vectors[i].input, strlen(vectors[i].input)) == 0);
        assert_digest(&ctx, vectors[i].digest);
    }
    char chunk[1000]; memset(chunk, 'a', sizeof chunk);
    visualptt_sha256_init(&ctx);
    for (int i = 0; i < 1000; i++) assert(visualptt_sha256_update(&ctx, chunk, sizeof chunk) == 0);
    assert_digest(&ctx, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    /* Binary/padding-boundary reference values independently generated with
     * GNU sha256sum, checked with different update boundaries and alignment. */
    const struct { size_t length; const char *digest; } binary[] = {
        {55,"463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56,"da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63,"29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64,"fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65,"4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {127,"92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
        {128,"471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
        {129,"5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
        {65537,"2deb0bd2129a9d3aed91e3cff58b3993752be549642890a3e853ec1065f9b617"}
    };
    unsigned char data[65538];
    for (size_t i = 0; i < sizeof data - 1; i++) data[i+1] = (unsigned char)i;
    const size_t chunks[] = {1, 7, 55, 64, 97, 65536, 65537};
    for (size_t i = 0; i < sizeof binary/sizeof *binary; i++) {
        for (size_t j = 0; j < sizeof chunks/sizeof *chunks; j++) {
            visualptt_sha256_init(&ctx);
            assert(visualptt_sha256_update(&ctx, NULL, 0) == 0);
            for (size_t offset = 0; offset < binary[i].length;) {
                size_t n = binary[i].length - offset;
                if (n > chunks[j]) n = chunks[j];
                assert(visualptt_sha256_update(&ctx, data + 1 + offset, n) == 0);
                offset += n;
            }
            assert_digest(&ctx, binary[i].digest);
        }
    }
    visualptt_sha256_init(&ctx);
    ctx.bytes = UINT64_MAX / 8;
    assert(visualptt_sha256_update(&ctx, "x", 1) == -1);
    puts("SHA-256 known-answer, binary, padding and streaming vectors passed.");
}

static void test_existing_sha256_pending(void)
{
    Fixture f; fixture(&f); start(&f);
    put(&f, "input/.visualptt-collector-00000000000000000000000000000000.part", "abc");
    assert(sql_exec(&f.collector,
        "INSERT INTO delivery VALUES('A','" NEW "',"
        "'.visualptt-collector-00000000000000000000000000000000.part',3,"
        "'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')") == 0);
    stop(&f); start(&f);
    contents(&f, "input/A_" NEW, "abc");
    assert(query_count(&f.collector, "SELECT count(*) FROM delivery WHERE temp IS NULL") == 1);
    cleanup(&f);
}

int main(void)
{
    umask(0077);
    test_sha256(); test_existing_sha256_pending();
    test_delivery_restart(); test_safe_sources(); test_source_change(1); test_source_change(2);
    test_short_copy(); test_collision(); test_destination_symlink();
    const char *stages[] = {"copy", "copied", "intent", "rename", "consumed", "committed"};
    for (size_t i = 0; i < sizeof stages/sizeof *stages; i++) test_crash(stages[i]);
    test_validation(); test_purge(); test_offline_history(); test_upgrade_ledger(); test_sql_failure(); test_cli(); test_daemon();
    printf("Native collector checks passed (%u fixtures, including six SIGKILL boundaries).\n", checks);
    return 0;
}
