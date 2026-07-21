/*
 * sds3sync — push finished SD-card recordings to S3-compatible storage.
 *
 * Trigger model: an in-app monotonic timer (IntervalSeconds, default 60)
 * starts one sync pass per tick: walk SourceDir, upload every finished
 * .mkv file (mtime older than MIN_AGE_SECONDS) that is not yet in the local
 * upload state, then record it so it is uploaded exactly once.
 * A tick that fires while a pass is still running is skipped. Files are
 * never deleted from the SD card — the camera's own FIFO cleanup reclaims
 * space.
 */
#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>

#include <axsdk/axparameter.h>
#include <curl/curl.h>

#include "s3put.h"

#define APP_NAME "sds3sync"

typedef struct {
    gchar *endpoint;
    gchar *bucket;
    gchar *region;
    gchar *access_key;
    gchar *secret_key;
    gchar *prefix;
    gchar *source_dir;
    gint interval;
    gboolean path_style;
    gboolean insecure;
} Config;

static Config cfg;
static GMainLoop *loop;
static GHashTable *uploaded; /* rel path (owned) -> size string (owned) */
static gchar *state_path;
static gint sync_running = 0;

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static gchar *get_param(AXParameter *p, const char *name, const char *fallback)
{
    gchar *v = NULL;
    GError *err = NULL;
    if (!ax_parameter_get(p, name, &v, &err)) {
        if (err)
            g_error_free(err);
        v = g_strdup(fallback);
    }
    if (v)
        g_strstrip(v);
    return v;
}

static gboolean param_yes(const gchar *v)
{
    return v && (g_ascii_strcasecmp(v, "yes") == 0 ||
                 g_ascii_strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
}

/* Best-effort unique identifier for this camera, used only as a fallback
 * when /etc/hostname cannot be read. Axis's own default hostname is
 * "axis-<mac without colons>", so this reproduces that convention from the
 * primary interface's MAC address. */
static gchar *mac_based_id(void)
{
    gchar *addr = NULL;
    if (!g_file_get_contents("/sys/class/net/eth0/address", &addr, NULL, NULL))
        return NULL;
    g_strstrip(addr);
    GString *s = g_string_new("axis-");
    for (const gchar *p = addr; *p; p++)
        if (*p != ':')
            g_string_append_c(s, g_ascii_tolower(*p));
    g_free(addr);
    return g_string_free(s, FALSE);
}

static gchar *derive_hostname_prefix(void)
{
    gchar *host = NULL;
    if (g_file_get_contents("/etc/hostname", &host, NULL, NULL)) {
        g_strstrip(host);
        if (host[0] != '\0')
            return g_strdup_printf("%s/", host);
        g_free(host);
    }

    gchar *mac_id = mac_based_id();
    if (mac_id != NULL)
        return g_strdup_printf("%s/", mac_id);

    syslog(LOG_WARNING,
           "could not read /etc/hostname or eth0 MAC — falling back to "
           "'unknown-camera/'; set Prefix manually to avoid colliding with "
           "other cameras in the bucket");
    return g_strdup("unknown-camera/");
}

/* If Prefix is unset, derive it from the camera's identity and persist it
 * so the UI reflects the real value on subsequent views/restarts. */
static void ensure_prefix(AXParameter *p)
{
    if (cfg.prefix[0] != '\0')
        return;

    gchar *derived = derive_hostname_prefix();
    GError *err = NULL;
    if (!ax_parameter_set(p, "Prefix", derived, TRUE, &err)) {
        syslog(LOG_WARNING, "could not persist auto-derived Prefix '%s': %s",
               derived, err ? err->message : "unknown");
        g_clear_error(&err);
    } else {
        syslog(LOG_INFO, "auto-derived Prefix on first run: '%s'", derived);
    }
    g_free(cfg.prefix);
    cfg.prefix = derived;
}

static gboolean load_config(void)
{
    GError *err = NULL;
    AXParameter *p = ax_parameter_new(APP_NAME, &err);
    if (p == NULL) {
        syslog(LOG_ERR, "ax_parameter_new failed: %s",
               err ? err->message : "unknown");
        g_clear_error(&err);
        return FALSE;
    }

    cfg.endpoint = get_param(p, "Endpoint", "");
    cfg.bucket = get_param(p, "Bucket", "cctv");
    cfg.region = get_param(p, "Region", "us-east-1");
    cfg.access_key = get_param(p, "AccessKey", "");
    cfg.secret_key = get_param(p, "SecretKey", "");
    cfg.prefix = get_param(p, "Prefix", "");
    cfg.source_dir = get_param(p, "SourceDir", "/var/spool/storage/SD_DISK");
    ensure_prefix(p);

    gchar *iv = get_param(p, "IntervalSeconds", "60");
    cfg.interval = atoi(iv);
    if (cfg.interval < 10)
        cfg.interval = 10; /* protect against hammering the SD card */
    g_free(iv);

    gchar *ps = get_param(p, "PathStyle", "yes");
    cfg.path_style = param_yes(ps);
    g_free(ps);

    gchar *ins = get_param(p, "InsecureTLS", "no");
    cfg.insecure = param_yes(ins);
    g_free(ins);

    /* strip trailing slashes so path joins stay predictable */
    size_t n = strlen(cfg.endpoint);
    while (n > 0 && cfg.endpoint[n - 1] == '/')
        cfg.endpoint[--n] = '\0';
    n = strlen(cfg.source_dir);
    while (n > 1 && cfg.source_dir[n - 1] == '/')
        cfg.source_dir[--n] = '\0';

    ax_parameter_free(p);

    if (cfg.endpoint[0] == '\0' || cfg.access_key[0] == '\0' ||
        cfg.secret_key[0] == '\0') {
        syslog(LOG_WARNING,
               "Endpoint/AccessKey/SecretKey not configured yet — "
               "set app parameters, then restart the app");
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Upload state (rel path -> size at upload time)                      */
/* ------------------------------------------------------------------ */

static void load_state(void)
{
    uploaded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gchar *content = NULL;
    if (!g_file_get_contents(state_path, &content, NULL, NULL))
        return;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *tab = strchr(*l, '\t');
        if (tab == NULL)
            continue;
        *tab = '\0';
        /* line: "<size>\t<rel path>"; later entries win */
        g_hash_table_replace(uploaded, g_strdup(tab + 1), g_strdup(*l));
    }
    g_strfreev(lines);
    g_free(content);
    syslog(LOG_INFO, "loaded %u uploaded-file records",
           g_hash_table_size(uploaded));
}

static void append_state(const gchar *rel, const gchar *size_str)
{
    FILE *fp = fopen(state_path, "a");
    if (fp == NULL) {
        syslog(LOG_ERR, "cannot append state file %s", state_path);
        return;
    }
    fprintf(fp, "%s\t%s\n", size_str, rel);
    fclose(fp);
}

/* Rewrite state_path from the current (already-pruned) hash table.
 * Atomic: write to a temp file, fflush, then rename over the original —
 * a crash mid-write leaves the previous state file untouched. */
static void rewrite_state_file(void)
{
    gchar *tmp_path = g_strdup_printf("%s.tmp", state_path);
    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        syslog(LOG_ERR, "reconcile: cannot open %s for rewrite", tmp_path);
        g_free(tmp_path);
        return;
    }

    GHashTableIter iter;
    gpointer key, value;
    gboolean write_error = FALSE;
    g_hash_table_iter_init(&iter, uploaded);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (fprintf(fp, "%s\t%s\n", (const gchar *)value, (const gchar *)key) < 0)
            write_error = TRUE;
    }

    if (write_error || fflush(fp) != 0) {
        syslog(LOG_ERR, "reconcile: write error rewriting %s, keeping old state file",
               tmp_path);
        fclose(fp);
        g_unlink(tmp_path);
        g_free(tmp_path);
        return;
    }
    fclose(fp);

    if (g_rename(tmp_path, state_path) != 0)
        syslog(LOG_ERR, "reconcile: rename %s -> %s failed", tmp_path, state_path);
    g_free(tmp_path);
}

/* Drop entries whose file no longer exists under SourceDir (already cycled
 * out by the camera's own FIFO cleanup), then compact the state file to
 * match. This is what keeps both the hash table and uploaded.txt bounded
 * by "what currently fits on the SD card" instead of growing forever.
 *
 * Guarded against a misconfigured/unmounted SourceDir: if the directory
 * itself isn't there, every entry would look "missing" and we'd wipe state
 * for files that are actually still on disk, so we bail out instead. */
static void reconcile_state(const char *why)
{
    if (!g_file_test(cfg.source_dir, G_FILE_TEST_IS_DIR)) {
        syslog(LOG_WARNING,
               "reconcile (%s) skipped: SourceDir '%s' is not accessible "
               "right now", why, cfg.source_dir);
        return;
    }

    guint before = g_hash_table_size(uploaded);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, uploaded);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        gchar *full = g_build_filename(cfg.source_dir, (const gchar *)key, NULL);
        gboolean exists = g_file_test(full, G_FILE_TEST_EXISTS);
        g_free(full);
        if (!exists)
            g_hash_table_iter_remove(&iter); /* safe: removes via the iterator */
    }
    guint removed = before - g_hash_table_size(uploaded);

    syslog(LOG_INFO, "reconcile (%s): removed %u stale entries, %u remain",
           why, removed, g_hash_table_size(uploaded));

    if (removed > 0)
        rewrite_state_file();
}

/* ------------------------------------------------------------------ */
/* Sync pass                                                           */
/* ------------------------------------------------------------------ */

#define RECONCILE_EVERY_UPLOADS 1024
static guint uploads_since_reconcile = 0;

static gchar *urlencode_key(const char *key)
{
    GString *s = g_string_new(NULL);
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        if (g_ascii_isalnum(*p) || *p == '-' || *p == '.' || *p == '_' ||
            *p == '~' || *p == '/')
            g_string_append_c(s, (gchar)*p);
        else
            g_string_append_printf(s, "%%%02X", *p);
    }
    return g_string_free(s, FALSE);
}

/* Axis edge storage keeps non-video artifacts (e.g. index.db) alongside
 * recordings in the same directory tree; this is what filters them out.
 * Hardcoded rather than a param: the container format is dictated by the
 * camera's own recording engine, not an operational choice, and a wrong
 * value here fails silently (zero uploads, indistinguishable from "no new
 * recordings"), so it isn't worth exposing as something to misconfigure. */
#define RECORDING_EXTENSION ".mkv"

static gboolean ext_allowed(const gchar *name)
{
    return g_str_has_suffix(name, RECORDING_EXTENSION);
}

/* Skip files still being actively written. Not exposed as a param: it's a
 * safety margin against the recording pipeline's own chunking/flush
 * behavior, not an operational choice, and a value set too low would
 * silently risk uploading partial files. */
#define MIN_AGE_SECONDS 120

struct pass_stats {
    guint uploaded;
    guint skipped;
    guint failed;
    guint consecutive_failures;
};

static void scan_dir(const gchar *dir, time_t now, const S3Cfg *s3,
                     struct pass_stats *st)
{
    if (st->consecutive_failures >= 3)
        return; /* endpoint is unhappy — stop the pass, retry on next pulse */

    GDir *d = g_dir_open(dir, 0, NULL);
    if (d == NULL)
        return;

    const gchar *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (st->consecutive_failures >= 3)
            break;

        gchar *full = g_build_filename(dir, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_SYMLINK)) {
            g_free(full);
            continue;
        }
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            scan_dir(full, now, s3, st);
            g_free(full);
            continue;
        }
        if (!ext_allowed(name)) {
            g_free(full);
            continue;
        }

        GStatBuf sb;
        if (g_stat(full, &sb) != 0 || !S_ISREG(sb.st_mode)) {
            g_free(full);
            continue;
        }
        if (now - sb.st_mtime < MIN_AGE_SECONDS) {
            /* likely still being written */
            g_free(full);
            continue;
        }

        const gchar *rel = full + strlen(cfg.source_dir);
        while (*rel == '/')
            rel++;

        gchar *size_str = g_strdup_printf("%" G_GINT64_FORMAT,
                                          (gint64)sb.st_size);
        const gchar *prev = g_hash_table_lookup(uploaded, rel);
        if (prev != NULL && strcmp(prev, size_str) == 0) {
            st->skipped++;
            g_free(size_str);
            g_free(full);
            continue;
        }

        gchar *key_plain = g_strconcat(cfg.prefix, rel, NULL);
        gchar *key_enc = urlencode_key(key_plain);
        long code = 0;
        char errbuf[512];
        int rc = s3_put_file(s3, key_enc, full, (gint64)sb.st_size, &code,
                             errbuf, sizeof(errbuf));
        if (rc == 0) {
            syslog(LOG_INFO, "uploaded %s (%s bytes)", rel, size_str);
            g_hash_table_replace(uploaded, g_strdup(rel), g_strdup(size_str));
            append_state(rel, size_str);
            st->uploaded++;
            st->consecutive_failures = 0;
            if (++uploads_since_reconcile >= RECONCILE_EVERY_UPLOADS) {
                reconcile_state("upload count");
                uploads_since_reconcile = 0;
            }
        } else {
            syslog(LOG_WARNING, "upload failed for %s (HTTP %ld): %s", rel,
                   code, errbuf);
            st->failed++;
            st->consecutive_failures++;
        }
        g_free(key_plain);
        g_free(key_enc);
        g_free(size_str);
        g_free(full);
    }
    g_dir_close(d);
}

static gpointer sync_thread(gpointer data)
{
    (void)data;
    S3Cfg s3 = {
        .endpoint = cfg.endpoint,
        .bucket = cfg.bucket,
        .region = cfg.region,
        .access_key = cfg.access_key,
        .secret_key = cfg.secret_key,
        .path_style = cfg.path_style,
        .insecure = cfg.insecure,
    };
    struct pass_stats st = { 0, 0, 0, 0 };
    time_t start = time(NULL);
    scan_dir(cfg.source_dir, start, &s3, &st);
    if (st.uploaded > 0 || st.failed > 0)
        syslog(LOG_INFO,
               "sync pass done in %lds: %u uploaded, %u already synced, "
               "%u failed",
               (long)(time(NULL) - start), st.uploaded, st.skipped, st.failed);
    g_atomic_int_set(&sync_running, 0);
    return NULL;
}

static void trigger_sync(const char *why)
{
    if (!g_atomic_int_compare_and_exchange(&sync_running, 0, 1)) {
        syslog(LOG_DEBUG, "sync already running, skipping trigger (%s)", why);
        return;
    }
    GThread *t = g_thread_new("sync", sync_thread, NULL);
    g_thread_unref(t);
}

/* ------------------------------------------------------------------ */

static gboolean initial_sync(gpointer data)
{
    (void)data;
    trigger_sync("startup");
    return G_SOURCE_REMOVE;
}

static gboolean on_timer(gpointer data)
{
    (void)data;
    trigger_sync("timer");
    return G_SOURCE_CONTINUE;
}

static gboolean on_quit_signal(gpointer data)
{
    (void)data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(void)
{
    openlog(APP_NAME, LOG_PID, LOG_USER);
    syslog(LOG_INFO, "starting");

    state_path =
        g_strdup_printf("/usr/local/packages/%s/localdata/uploaded.txt",
                        APP_NAME);

    gboolean configured = load_config();
    load_state();
    reconcile_state("startup"); /* prune cruft accumulated across restarts */
    curl_global_init(CURL_GLOBAL_ALL);

    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_quit_signal, NULL);
    g_unix_signal_add(SIGINT, on_quit_signal, NULL);

    if (configured) {
        /* one pass shortly after start, then steady cadence */
        g_timeout_add_seconds(15, initial_sync, NULL);
        g_timeout_add_seconds((guint)cfg.interval, on_timer, NULL);
        syslog(LOG_INFO, "timer armed: sync pass every %d s", cfg.interval);
    } else {
        /* stay alive so the parameter page works; user restarts app after
         * configuring */
        syslog(LOG_INFO, "idle: waiting for configuration");
    }

    g_main_loop_run(loop);

    syslog(LOG_INFO, "stopping");
    g_main_loop_unref(loop);
    curl_global_cleanup();
    return EXIT_SUCCESS;
}
