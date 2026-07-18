#include "sigv4.h"

#include <glib.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <string.h>

static void to_hex(const unsigned char *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static void sha256_hex(const char *data, char out[65])
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)data, strlen(data), digest);
    to_hex(digest, sizeof(digest), out);
}

static void hmac256(const void *key, size_t key_len,
                    const void *data, size_t data_len,
                    unsigned char out[32])
{
    unsigned int n = 32;
    HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, &n);
}

char *sigv4_authorization(const char *method,
                          const char *canonical_uri,
                          const char *host,
                          const char *region,
                          const char *access_key,
                          const char *secret_key,
                          const char *amz_date,
                          const char *payload_hash)
{
    char date[9];
    memcpy(date, amz_date, 8);
    date[8] = '\0';

    const char *signed_headers = "host;x-amz-content-sha256;x-amz-date";

    gchar *canonical_headers = g_strdup_printf(
        "host:%s\nx-amz-content-sha256:%s\nx-amz-date:%s\n",
        host, payload_hash, amz_date);

    /* method \n uri \n query(empty) \n headers \n signed \n payload-hash */
    gchar *canonical_request = g_strdup_printf(
        "%s\n%s\n\n%s\n%s\n%s",
        method, canonical_uri, canonical_headers, signed_headers, payload_hash);

    char cr_hash[65];
    sha256_hex(canonical_request, cr_hash);

    gchar *scope = g_strdup_printf("%s/%s/s3/aws4_request", date, region);
    gchar *string_to_sign = g_strdup_printf(
        "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, scope, cr_hash);

    gchar *k_secret = g_strdup_printf("AWS4%s", secret_key);
    unsigned char k_date[32], k_region[32], k_service[32], k_signing[32], sig[32];
    hmac256(k_secret, strlen(k_secret), date, 8, k_date);
    hmac256(k_date, 32, region, strlen(region), k_region);
    hmac256(k_region, 32, "s3", 2, k_service);
    hmac256(k_service, 32, "aws4_request", 12, k_signing);
    hmac256(k_signing, 32, string_to_sign, strlen(string_to_sign), sig);

    char sig_hex[65];
    to_hex(sig, 32, sig_hex);

    char *auth = g_strdup_printf(
        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
        access_key, scope, signed_headers, sig_hex);

    g_free(canonical_headers);
    g_free(canonical_request);
    g_free(scope);
    g_free(string_to_sign);
    g_free(k_secret);
    return auth;
}
