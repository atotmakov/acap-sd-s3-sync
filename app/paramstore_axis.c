/* Real backing: thin wrapper around Axis's axparameter library. Compiled
 * only into the aarch64 on-camera build -- see app/Makefile. */
#include "paramstore.h"

#include <axsdk/axparameter.h>
#include <syslog.h>

struct ParamStore {
    AXParameter *p;
};

ParamStore *paramstore_open(const char *app_name)
{
    GError *err = NULL;
    AXParameter *p = ax_parameter_new(app_name, &err);
    if (p == NULL) {
        syslog(LOG_ERR, "ax_parameter_new failed: %s",
               err ? err->message : "unknown");
        g_clear_error(&err);
        return NULL;
    }
    ParamStore *ps = g_new0(ParamStore, 1);
    ps->p = p;
    return ps;
}

gchar *paramstore_get(ParamStore *ps, const char *name, const char *fallback)
{
    gchar *v = NULL;
    GError *err = NULL;
    if (!ax_parameter_get(ps->p, name, &v, &err)) {
        if (err)
            g_error_free(err);
        v = g_strdup(fallback);
    }
    if (v)
        g_strstrip(v);
    return v;
}

gboolean paramstore_set(ParamStore *ps, const char *name, const char *value)
{
    GError *err = NULL;
    if (!ax_parameter_set(ps->p, name, value, TRUE, &err)) {
        syslog(LOG_WARNING, "paramstore_set(%s) failed: %s", name,
               err ? err->message : "unknown");
        g_clear_error(&err);
        return FALSE;
    }
    return TRUE;
}

void paramstore_close(ParamStore *ps)
{
    if (ps == NULL)
        return;
    ax_parameter_free(ps->p);
    g_free(ps);
}
