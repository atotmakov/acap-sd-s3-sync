/* Host backing for integration tests: a flat key=value file, so set()
 * genuinely persists across a real process restart the same way an
 * on-device ax_parameter_set(..., do_sync=TRUE, ...) does. Path from
 * SDS3SYNC_PARAM_FILE, defaulting to ./sds3sync-params.conf in the
 * current working directory so a test harness can point each run at its
 * own scratch file. Compiled only into host builds -- see app/Makefile. */
#include "paramstore.h"

#include <string.h>

struct ParamStore {
    gchar *path;
    GHashTable *kv; /* owned key/value strings */
};

static void load_file(ParamStore *ps)
{
    gchar *content = NULL;
    if (!g_file_get_contents(ps->path, &content, NULL, NULL))
        return;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        gchar *eq = strchr(*l, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        g_hash_table_replace(ps->kv, g_strdup(*l), g_strdup(eq + 1));
    }
    g_strfreev(lines);
    g_free(content);
}

static void save_file(ParamStore *ps)
{
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ps->kv);
    while (g_hash_table_iter_next(&iter, &key, &value))
        g_string_append_printf(out, "%s=%s\n", (const gchar *)key,
                               (const gchar *)value);
    g_file_set_contents(ps->path, out->str, out->len, NULL);
    g_string_free(out, TRUE);
}

ParamStore *paramstore_open(const char *app_name)
{
    (void)app_name;
    ParamStore *ps = g_new0(ParamStore, 1);
    const char *env_path = g_getenv("SDS3SYNC_PARAM_FILE");
    ps->path = g_strdup(env_path != NULL ? env_path : "./sds3sync-params.conf");
    ps->kv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    load_file(ps);
    return ps;
}

gchar *paramstore_get(ParamStore *ps, const char *name, const char *fallback)
{
    const gchar *v = g_hash_table_lookup(ps->kv, name);
    gchar *out = g_strdup(v != NULL ? v : fallback);
    if (out)
        g_strstrip(out);
    return out;
}

gboolean paramstore_set(ParamStore *ps, const char *name, const char *value)
{
    g_hash_table_replace(ps->kv, g_strdup(name), g_strdup(value));
    save_file(ps);
    return TRUE;
}

void paramstore_close(ParamStore *ps)
{
    if (ps == NULL)
        return;
    g_hash_table_destroy(ps->kv);
    g_free(ps->path);
    g_free(ps);
}
