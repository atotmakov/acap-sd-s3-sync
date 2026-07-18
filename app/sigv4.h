#ifndef SIGV4_H
#define SIGV4_H

/*
 * Build an AWS Signature Version 4 Authorization header value for an S3
 * request signed with UNSIGNED-PAYLOAD (no body hash) over exactly three
 * signed headers: host, x-amz-content-sha256, x-amz-date.
 *
 * canonical_uri must already be URL-encoded and start with '/'.
 * amz_date is "YYYYMMDDTHHMMSSZ" (UTC).
 * Returns a newly allocated string (g_free it).
 */
char *sigv4_authorization(const char *method,
                          const char *canonical_uri,
                          const char *host,
                          const char *region,
                          const char *access_key,
                          const char *secret_key,
                          const char *amz_date,
                          const char *payload_hash);

#endif
