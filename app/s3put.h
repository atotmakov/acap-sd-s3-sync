#ifndef S3PUT_H
#define S3PUT_H

#include <glib.h>

typedef struct {
    const char *endpoint;    /* "https://host[:port]" — no trailing slash */
    const char *bucket;
    const char *region;
    const char *access_key;
    const char *secret_key;
    gboolean path_style;     /* TRUE: endpoint/bucket/key (MinIO), FALSE: bucket.host/key */
    gboolean insecure;       /* TRUE: skip TLS certificate verification */
} S3Cfg;

/*
 * PUT a local file to the bucket under enc_key (URL-encoded object key,
 * no leading slash). Returns 0 on HTTP 2xx. On failure *http_code holds the
 * HTTP status (0 if transport error) and errbuf a short diagnostic.
 */
int s3_put_file(const S3Cfg *c, const char *enc_key, const char *filepath,
                gint64 size, long *http_code, char *errbuf, size_t errlen);

#endif
