#ifndef PARAMSTORE_H
#define PARAMSTORE_H

#include <glib.h>

/*
 * Minimal seam around parameter storage. On-camera (aarch64 build,
 * paramstore_axis.c) this is a thin wrapper around Axis's axparameter
 * library. On host builds (paramstore_host.c) it's backed by a flat
 * key=value file so paramstore_set() genuinely persists across a real
 * process restart, mirroring on-device parameter persistence -- this is
 * what lets integration tests exercise the same Prefix-auto-derive-and-
 * persist behavior (see ensure_prefix() in sdsync.c) that runs on a real
 * camera, without axparameter itself (which only exists for armv7hf/
 * aarch64 -- there is no x86_64 build of it).
 */
typedef struct ParamStore ParamStore;

/* Opens the parameter store for app_name. Returns NULL on failure. */
ParamStore *paramstore_open(const char *app_name);

/* Returns a newly-allocated (g_free) copy of the parameter value, stripped
 * of surrounding whitespace, or a newly-allocated copy of fallback if the
 * parameter is unset or unreadable. */
gchar *paramstore_get(ParamStore *ps, const char *name, const char *fallback);

/* Sets and persists a parameter value. Returns TRUE on success. */
gboolean paramstore_set(ParamStore *ps, const char *name, const char *value);

void paramstore_close(ParamStore *ps);

#endif
