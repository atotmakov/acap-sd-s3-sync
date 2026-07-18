#include "s3put.h"
#include "sigv4.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct resp_buf {
    char *buf;
    size_t len;
    size_t cap;
};

static size_t on_body(char *ptr, size_t sz, size_t nmemb, void *ud)
{
    struct resp_buf *r = ud;
    size_t n = sz * nmemb;
    size_t room = (r->cap > r->len + 1) ? r->cap - r->len - 1 : 0;
    size_t copy = n < room ? n : room;
    if (copy > 0) {
        memcpy(r->buf + r->len, ptr, copy);
        r->len += copy;
        r->buf[r->len] = '\0';
    }
    return n; /* always consume everything so the transfer completes */
}

int s3_put_file(const S3Cfg *c, const char *enc_key, const char *filepath,
                gint64 size, long *http_code, char *errbuf, size_t errlen)
{
    *http_code = 0;
    if (errlen > 0)
        errbuf[0] = '\0';

    const char *scheme_end = strstr(c->endpoint, "://");
    if (scheme_end == NULL) {
        g_snprintf(errbuf, errlen, "endpoint missing scheme: %s", c->endpoint);
        return -1;
    }
    gchar *scheme = g_strndup(c->endpoint, (gsize)(scheme_end - c->endpoint));
    const char *hostpart = scheme_end + 3;

    gchar *host, *canonical_uri;
    if (c->path_style) {
        host = g_strdup(hostpart);
        canonical_uri = g_strdup_printf("/%s/%s", c->bucket, enc_key);
    } else {
        host = g_strdup_printf("%s.%s", c->bucket, hostpart);
        canonical_uri = g_strdup_printf("/%s", enc_key);
    }
    gchar *url = g_strdup_printf("%s://%s%s", scheme, host, canonical_uri);

    char amz_date[20];
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &tm_utc);

    gchar *auth = sigv4_authorization("PUT", canonical_uri, host, c->region,
                                      c->access_key, c->secret_key,
                                      amz_date, "UNSIGNED-PAYLOAD");

    int ret = -1;
    FILE *fp = fopen(filepath, "rb");
    if (fp == NULL) {
        g_snprintf(errbuf, errlen, "cannot open %s", filepath);
        goto out_nofile;
    }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        g_snprintf(errbuf, errlen, "curl_easy_init failed");
        goto out;
    }

    struct curl_slist *headers = NULL;
    gchar *h_date = g_strdup_printf("x-amz-date: %s", amz_date);
    gchar *h_auth = g_strdup_printf("Authorization: %s", auth);
    headers = curl_slist_append(headers, h_date);
    headers = curl_slist_append(headers, "x-amz-content-sha256: UNSIGNED-PAYLOAD");
    headers = curl_slist_append(headers, h_auth);
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    headers = curl_slist_append(headers, "Expect:");

    char body_store[512];
    struct resp_buf body = { body_store, 0, sizeof(body_store) };
    char curl_err[CURL_ERROR_SIZE] = "";

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, fp);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    /* abort if slower than 1 KiB/s for 60 s rather than a hard total timeout,
     * since recording chunks can be large */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (c->insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode cc = curl_easy_perform(curl);
    if (cc != CURLE_OK) {
        g_snprintf(errbuf, errlen, "curl: %s",
                   curl_err[0] ? curl_err : curl_easy_strerror(cc));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
        if (*http_code >= 200 && *http_code < 300) {
            ret = 0;
        } else {
            g_snprintf(errbuf, errlen, "%s", body.len ? body.buf : "(no body)");
        }
    }

    curl_slist_free_all(headers);
    g_free(h_date);
    g_free(h_auth);
    curl_easy_cleanup(curl);
out:
    fclose(fp);
out_nofile:
    g_free(scheme);
    g_free(host);
    g_free(canonical_uri);
    g_free(url);
    g_free(auth);
    return ret;
}
