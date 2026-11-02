#define _GNU_SOURCE
#include "mi_storage.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mi_util.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sbuf;

static int sbuf_append(sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t want = (b->len + n + 1) * 2;
        char *grown = mi_realloc(b->data, want);
        if (!grown) return -1;
        b->data = grown;
        b->cap = want;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int sbuf_puts(sbuf *b, const char *s) { return sbuf_append(b, s, strlen(s)); }

static size_t curl_to_sbuf(char *ptr, size_t size, size_t nmemb, void *userdata) {
    sbuf *b = userdata;
    if (sbuf_append(b, ptr, size * nmemb) != 0) return 0;
    return size * nmemb;
}

/* Capture the ETag response header (case-insensitive). */
static size_t header_etag(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    if (n > 5 && !strncasecmp(ptr, "ETag:", 5)) {
        char **slot = userdata;
        free(*slot);
        const char *v = ptr + 5;
        size_t vlen = n - 5;
        while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' ||
                            v[vlen - 1] == ' '))
            vlen--;
        while (vlen > 0 && *v == ' ') {
            v++;
            vlen--;
        }
        *slot = mi_strndup(v, vlen);
    }
    return n;
}

/* Read a whole file into memory. Returns 0 and sets *out / *out_len. */
static int read_whole_file(const char *path, unsigned char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return MI_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return MI_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return MI_ERR_IO;
    }
    rewind(f);
    unsigned char *buf = mi_malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return MI_ERR_NOMEM;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return MI_ERR_IO;
    }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return MI_OK;
}

/* ------------------------------------------------------------------ */
/* AWS SigV4                                                           */
/* ------------------------------------------------------------------ */

/* RFC 3986 encoding for a single path segment (keep '/'). */
static char *uri_encode(const char *s, int keep_slash) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *out = mi_malloc(n * 3 + 1);
    if (!out) return NULL;
    char *w = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~' ||
            (keep_slash && *p == '/')) {
            *w++ = (char)*p;
        } else {
            *w++ = '%';
            *w++ = hex[*p >> 4];
            *w++ = hex[*p & 0xF];
        }
    }
    *w = '\0';
    return out;
}

/* Hostname only (no port) — used to build virtual-host bucket URLs. */
static char *host_from_endpoint(const char *endpoint) {
    const char *p = endpoint;
    if (!strncmp(p, "https://", 8)) p += 8;
    else if (!strncmp(p, "http://", 7)) p += 7;
    size_t n = strcspn(p, "/:");
    return mi_strndup(p, n);
}

/* Authority (host[:port]) — this is what goes in the Host header and what must
 * be signed, so it has to match the wire exactly (libcurl sends the port when
 * it is non-default, e.g. MinIO on :9000). */
static char *authority_from_endpoint(const char *endpoint) {
    const char *p = endpoint;
    if (!strncmp(p, "https://", 8)) p += 8;
    else if (!strncmp(p, "http://", 7)) p += 7;
    size_t n = strcspn(p, "/");
    return mi_strndup(p, n);
}

typedef struct {
    char *canonical_uri;
    char *host;         /* host[:port] as it appears in the Host header */
    char *amz_date;
    char *date_stamp;
    char *payload_hash; /* lowercase hex */
} sig_inputs;

static int compute_signing_key(const mi_s3_config *cfg, const char *date_stamp,
                               unsigned char out_key[32]) {
    char *k0 = mi_asprintf("AWS4%s", cfg->secret_key);
    if (!k0) return -1;
    unsigned char k1[32], k2[32], k3[32];
    mi_hmac_sha256(k0, strlen(k0), date_stamp, strlen(date_stamp), k1);
    free(k0);
    mi_hmac_sha256(k1, 32, cfg->region, strlen(cfg->region), k2);
    mi_hmac_sha256(k2, 32, "s3", 2, k3);
    mi_hmac_sha256(k3, 32, "aws4_request", 12, out_key);
    return 0;
}

/* Produce the value of the Authorization header. Caller frees. */
static char *build_authorization(const mi_s3_config *cfg,
                                 const sig_inputs *in,
                                 const char *signed_headers,
                                 const char *canonical_headers,
                                 const char *canonical_query) {
    /* Canonical request: METHOD\nURI\nQUERY\nCANONICAL_HEADERS\nSIGNED_HEADERS\nPAYLOAD_HASH
     * (canonical_headers ends with its own newline; AWS still expects the
     * blank line separator before the signed-header list). */
    char *canonical_request =
        mi_asprintf("PUT\n%s\n%s\n%s\n%s\n%s", in->canonical_uri,
                    canonical_query ? canonical_query : "", canonical_headers,
                    signed_headers, in->payload_hash);
    if (!canonical_request) return NULL;

    char creq_hash[65];
    mi_sha256_hex(canonical_request, strlen(canonical_request), creq_hash);
    free(canonical_request);

    char *scope = mi_asprintf("%s/%s/s3/aws4_request", in->date_stamp, cfg->region);
    if (!scope) return NULL;
    char *sts = mi_asprintf("AWS4-HMAC-SHA256\n%s\n%s\n%s", in->amz_date, scope,
                            creq_hash);
    if (!sts) {
        free(scope);
        return NULL;
    }

    unsigned char signing_key[32];
    if (compute_signing_key(cfg, in->date_stamp, signing_key) != 0) {
        free(scope);
        free(sts);
        return NULL;
    }
    unsigned char sig[32];
    mi_hmac_sha256(signing_key, 32, sts, strlen(sts), sig);
    char sig_hex[65];
    mi_hex_encode(sig, 32, sig_hex);

    char *auth = mi_asprintf(
        "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
        cfg->access_key, scope, signed_headers, sig_hex);
    free(scope);
    free(sts);
    return auth;
}

/* ------------------------------------------------------------------ */
/* upload                                                              */
/* ------------------------------------------------------------------ */

void mi_s3_config_init(mi_s3_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->endpoint = "https://s3.amazonaws.com";
    cfg->region = "us-east-1";
    cfg->path_style = 1;
    cfg->timeout_seconds = 300;
    cfg->verify_tls = 1;
}

void mi_cdn_config_init(mi_cdn_config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->timeout_seconds = 30;
}

void mi_upload_result_free(mi_upload_result *res) {
    if (!res) return;
    free(res->url);
    free(res->etag);
    memset(res, 0, sizeof *res);
}

void mi_cdn_result_free(mi_cdn_result *res) {
    if (!res) return;
    free(res->body);
    memset(res, 0, sizeof *res);
}

/* Infer a Content-Type from the object key's extension. */
static const char *content_type_for_key(const char *key) {
    const char *dot = strrchr(key, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".m3u8")) return "application/vnd.apple.mpegurl";
    if (!strcmp(dot, ".ts")) return "video/mp2t";
    if (!strcmp(dot, ".mp4") || !strcmp(dot, ".m4v")) return "video/mp4";
    if (!strcmp(dot, ".webm")) return "video/webm";
    if (!strcmp(dot, ".mkv")) return "video/x-matroska";
    if (!strcmp(dot, ".mov")) return "video/quicktime";
    if (!strcmp(dot, ".mpd")) return "application/dash+xml";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".vtt")) return "text/vtt";
    return "application/octet-stream";
}

static int ensure_curl(void) {
    static int done = 0;
    static int rc = -1;
    if (!done) {
        rc = curl_global_init(CURL_GLOBAL_DEFAULT) == 0 ? 0 : -1;
        done = 1;
    }
    return rc;
}

int mi_s3_upload_bytes(const mi_s3_config *cfg, const void *data, size_t len,
                       const char *object_key, mi_upload_result *out) {
    memset(out, 0, sizeof *out);
    if (ensure_curl() != 0) return MI_ERR;
    if (!cfg->bucket || !cfg->access_key || !cfg->secret_key || !object_key)
        return MI_ERR_ARG;

    char amz[17], date[9];
    mi_amz_timestamps(time(NULL), amz, date);

    char payload_hash[65];
    mi_sha256_hex(data, len, payload_hash);

    char *enc_key = uri_encode(object_key, 1);
    if (!enc_key) return MI_ERR_NOMEM;

    char *host = authority_from_endpoint(cfg->endpoint);
    if (!host) {
        free(enc_key);
        return MI_ERR_NOMEM;
    }

    char *url = NULL;
    char *canonical_uri = NULL;
    char *host_header = NULL;
    if (cfg->path_style) {
        url = mi_asprintf("%s/%s/%s", cfg->endpoint, cfg->bucket, enc_key);
        canonical_uri = mi_asprintf("/%s/%s", cfg->bucket, enc_key);
        host_header = mi_strdup(host);
    } else {
        char *bare = host_from_endpoint(cfg->endpoint);
        url = mi_asprintf("https://%s.%s/%s", cfg->bucket, bare, enc_key);
        canonical_uri = mi_asprintf("/%s", enc_key);
        host_header = mi_asprintf("%s.%s", cfg->bucket, bare);
        free(bare);
    }
    if (!url || !canonical_uri || !host_header) {
        free(enc_key); free(host); free(url); free(canonical_uri); free(host_header);
        return MI_ERR_NOMEM;
    }

    const char *content_type =
        cfg->content_type ? cfg->content_type : content_type_for_key(object_key);

    /* Canonical headers must be sorted by lowercase name. */
    sbuf canon = {0};
    sbuf signed_h = {0};
    sbuf_append(&canon, "", 0);
    /* content-type */
    sbuf_puts(&canon, "content-type:");
    sbuf_puts(&canon, content_type);
    sbuf_puts(&canon, "\n");
    sbuf_puts(&signed_h, "content-type;");
    /* host */
    sbuf_puts(&canon, "host:");
    sbuf_puts(&canon, host_header);
    sbuf_puts(&canon, "\n");
    sbuf_puts(&signed_h, "host;");
    /* x-amz-content-sha256 */
    sbuf_puts(&canon, "x-amz-content-sha256:");
    sbuf_puts(&canon, payload_hash);
    sbuf_puts(&canon, "\n");
    sbuf_puts(&signed_h, "x-amz-content-sha256;");
    /* x-amz-date */
    sbuf_puts(&canon, "x-amz-date:");
    sbuf_puts(&canon, amz);
    sbuf_puts(&canon, "\n");
    sbuf_puts(&signed_h, "x-amz-date");
    if (cfg->session_token) {
        sbuf_puts(&canon, ";x-amz-security-token:");
        sbuf_puts(&canon, cfg->session_token);
        sbuf_puts(&canon, "\n");
        sbuf_puts(&signed_h, ";x-amz-security-token");
    }

    sig_inputs in = {.canonical_uri = canonical_uri,
                     .host = host_header,
                     .amz_date = amz,
                     .date_stamp = date,
                     .payload_hash = payload_hash};
    char *auth = build_authorization(cfg, &in, signed_h.data, canon.data, "");
    if (!auth) {
        free(enc_key); free(host); free(url); free(canonical_uri);
        free(host_header); free(canon.data); free(signed_h.data);
        return MI_ERR;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(enc_key); free(host); free(url); free(canonical_uri);
        free(host_header); free(canon.data); free(signed_h.data); free(auth);
        return MI_ERR;
    }

    struct curl_slist *headers = NULL;
    char *h_amz = mi_asprintf("x-amz-date: %s", amz);
    char *h_hash = mi_asprintf("x-amz-content-sha256: %s", payload_hash);
    char *h_auth = mi_asprintf("Authorization: %s", auth);
    char *h_ct = mi_asprintf("Content-Type: %s", content_type);
    /* Pin the Host header to exactly what we signed; otherwise libcurl's own
     * Host (which includes a non-default port) would not match the signature. */
    char *h_host = mi_asprintf("Host: %s", host_header);
    headers = curl_slist_append(headers, h_amz);
    headers = curl_slist_append(headers, h_hash);
    headers = curl_slist_append(headers, h_auth);
    headers = curl_slist_append(headers, h_ct);
    headers = curl_slist_append(headers, h_host);
    if (cfg->session_token) {
        char *h_tok = mi_asprintf("x-amz-security-token: %s", cfg->session_token);
        headers = curl_slist_append(headers, h_tok);
        free(h_tok);
    }

    sbuf resp = {0};
    char *etag = NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_to_sbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_etag);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &etag);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, cfg->verify_tls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, cfg->verify_tls ? 2L : 0L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    int ret = MI_OK;
    if (rc != CURLE_OK) {
        mi_logf(MI_LOG_ERROR, "upload transport error: %s", curl_easy_strerror(rc));
        ret = MI_ERR_NET;
    } else if (status < 200 || status >= 300) {
        mi_logf(MI_LOG_ERROR, "upload failed: HTTP %ld %s", status,
                resp.data ? resp.data : "");
        ret = MI_ERR;
    } else {
        out->url = mi_strdup(url);
        out->etag = etag; /* transfer ownership */
        etag = NULL;
        out->bytes = (int64_t)len;
    }
    out->http_status = status;
    free(etag);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(h_amz); free(h_hash); free(h_auth); free(h_ct); free(h_host);
    free(resp.data);
    free(enc_key); free(host); free(url); free(canonical_uri); free(host_header);
    free(canon.data); free(signed_h.data); free(auth);
    if (ret != MI_OK) mi_upload_result_free(out);
    return ret;
}

int mi_s3_upload_file(const mi_s3_config *cfg, const char *local_path,
                      const char *object_key, mi_upload_result *out) {
    unsigned char *data = NULL;
    size_t len = 0;
    int rc = read_whole_file(local_path, &data, &len);
    if (rc != MI_OK) return rc;
    rc = mi_s3_upload_bytes(cfg, data, len, object_key, out);
    free(data);
    return rc;
}

char *mi_storage_public_url(const mi_s3_config *cfg, const char *cdn_base,
                            const char *object_key) {
    char *enc = uri_encode(object_key, 1);
    if (!enc) return NULL;
    char *url;
    if (cdn_base && *cdn_base) {
        size_t n = strlen(cdn_base);
        int slash = n && cdn_base[n - 1] == '/';
        url = mi_asprintf("%s%s%s", cdn_base, slash ? "" : "/", enc);
    } else if (cfg && cfg->bucket) {
        char *host = host_from_endpoint(cfg->endpoint);
        url = mi_asprintf("https://%s.%s/%s", cfg->bucket, host, enc);
        free(host);
    } else {
        url = NULL;
    }
    free(enc);
    return url;
}

/* ------------------------------------------------------------------ */
/* Cloudflare cache purge                                              */
/* ------------------------------------------------------------------ */

int mi_cdn_purge_urls(const mi_cdn_config *cfg, const char *const *urls,
                      size_t count, mi_cdn_result *out) {
    memset(out, 0, sizeof *out);
    if (ensure_curl() != 0) return MI_ERR;
    if (!cfg->api_token || !cfg->zone_id || count == 0) return MI_ERR_ARG;

    sbuf json = {0};
    sbuf_puts(&json, "{\"files\":[");
    for (size_t i = 0; i < count; i++) {
        if (i) sbuf_puts(&json, ",");
        sbuf_puts(&json, "\"");
        /* Minimal JSON escaping for URLs. */
        for (const char *p = urls[i]; p && *p; p++) {
            if (*p == '"' || *p == '\\') {
                char esc[3] = {'\\', *p, '\0'};
                sbuf_puts(&json, esc);
            } else {
                sbuf_append(&json, p, 1);
            }
        }
        sbuf_puts(&json, "\"");
    }
    sbuf_puts(&json, "]}");

    char *url = mi_asprintf(
        "https://api.cloudflare.com/client/v4/zones/%s/purge_cache",
        cfg->zone_id);
    if (!url) {
        free(json.data);
        return MI_ERR_NOMEM;
    }
    char *auth = mi_asprintf("Authorization: Bearer %s", cfg->api_token);

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(url); free(auth); free(json.data);
        return MI_ERR;
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    sbuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json.len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_to_sbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg->timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    out->http_status = status;
    out->body = resp.data; /* transfer ownership */
    int ret = MI_OK;
    if (rc != CURLE_OK) {
        mi_logf(MI_LOG_ERROR, "cdn purge transport error: %s",
                curl_easy_strerror(rc));
        ret = MI_ERR_NET;
    } else if (status < 200 || status >= 300) {
        mi_logf(MI_LOG_ERROR, "cdn purge failed: HTTP %ld %s", status,
                resp.data ? resp.data : "");
        ret = MI_ERR;
    } else {
        out->success = 1;
    }
    if (ret != MI_OK) {
        free(out->body);
        out->body = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(url); free(auth); free(json.data);
    return ret;
}
