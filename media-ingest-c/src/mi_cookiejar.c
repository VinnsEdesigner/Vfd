#define _GNU_SOURCE
#include "mi_cookiejar.h"
#include "mi_json.h"
#include "mi_util.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct mi_cookiejar {
    mi_cookie *cookies;
    size_t count, cap;
    pthread_mutex_t lock;
};

mi_cookiejar *mi_cookiejar_new(void) {
    mi_cookiejar *j = mi_calloc(1, sizeof *j);
    if (!j) return NULL;
    pthread_mutex_init(&j->lock, NULL);
    return j;
}

void mi_cookiejar_free(mi_cookiejar *jar) {
    if (!jar) return;
    for (size_t i = 0; i < jar->count; i++) {
        free(jar->cookies[i].name);
        free(jar->cookies[i].value);
        free(jar->cookies[i].domain);
        free(jar->cookies[i].path);
    }
    free(jar->cookies);
    pthread_mutex_destroy(&jar->lock);
    free(jar);
}

/* ------------------------------------------------------------------ */
/* URL helpers                                                         */
/* ------------------------------------------------------------------ */

/* The host of a URL, lowercased, without userinfo or port. */
static char *url_host(const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) p = at + 1;
    const char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
    if (end == p) return NULL;
    char *h = mi_strndup(p, (size_t)(end - p));
    for (char *q = h; q && *q; q++) *q = (char)tolower((unsigned char)*q);
    return h;
}

/* The path of a URL, defaulting to "/". */
static char *url_path(const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    if (!slash) return mi_strdup("/");
    const char *end = slash;
    while (*end && *end != '?' && *end != '#') end++;
    if (end == slash) return mi_strdup("/");
    return mi_strndup(slash, (size_t)(end - slash));
}

/* Whether `host` is `domain` or a subdomain of it. */
static int domain_matches(const char *host, const char *domain) {
    if (!host || !domain) return 0;
    size_t hl = strlen(host), dl = strlen(domain);
    if (hl == dl) return !strcasecmp(host, domain);
    if (hl > dl && host[hl - dl - 1] == '.' && !strcasecmp(host + hl - dl, domain))
        return 1;
    return 0;
}

/* Whether a request path is under a cookie path (RFC 6265 §5.1.4). */
static int path_matches(const char *req, const char *cookie_path) {
    if (!req || !cookie_path) return 0;
    size_t rl = strlen(req), cl = strlen(cookie_path);
    if (rl < cl) return 0;
    if (strncmp(req, cookie_path, cl) != 0) return 0;
    if (cl > 0 && cookie_path[cl - 1] == '/') return 1;
    return req[cl] == 0 || req[cl] == '/';
}

/* ------------------------------------------------------------------ */
/* Storing                                                             */
/* ------------------------------------------------------------------ */

/* Trim leading/trailing whitespace in place. */
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
}

/* The default path for a cookie set at `request_path` (RFC 6265 §5.1.4). */
static char *default_path(const char *request_path) {
    if (!request_path || request_path[0] != '/') return mi_strdup("/");
    const char *last = strrchr(request_path, '/');
    if (!last || last == request_path) return mi_strdup("/");
    return mi_strndup(request_path, (size_t)(last - request_path));
}

void mi_cookiejar_set(mi_cookiejar *jar, const char *set_cookie,
                      const char *request_host, const char *request_path) {
    if (!jar || !set_cookie || !*set_cookie) return;

    char *line = mi_strdup(set_cookie);
    if (!line) return;

    /* name=value; attr=val; ... */
    char *semi = strchr(line, ';');
    char *pair = line;
    if (semi) *semi = 0;

    char *eq = strchr(pair, '=');
    if (!eq) { free(line); return; }
    *eq = 0;
    char *name = pair;
    char *value = eq + 1;
    trim(name);
    trim(value);
    if (!*name) { free(line); return; }

    char *domain = NULL, *path = NULL;
    long expires = 0;
    int secure = 0, http_only = 0;

    /* The attributes. */
    char *attr = semi ? semi + 1 : NULL;
    while (attr && *attr) {
        char *next = strchr(attr, ';');
        if (next) *next = 0;
        char *aeq = strchr(attr, '=');
        char *an = attr;
        char *av = NULL;
        if (aeq) { *aeq = 0; av = aeq + 1; trim(av); }
        trim(an);
        if (!strcasecmp(an, "domain") && av && *av) {
            free(domain);
            const char *d = av;
            if (*d == '.') d++; /* a leading dot is the old "any subdomain" */
            domain = mi_strdup(d);
            for (char *q = domain; q && *q; q++) *q = (char)tolower((unsigned char)*q);
        } else if (!strcasecmp(an, "path") && av) {
            free(path);
            path = mi_strdup(av);
        } else if (!strcasecmp(an, "max-age") && av) {
            long secs = atol(av);
            expires = secs <= 0 ? 1 : (long)time(NULL) + secs;
        } else if (!strcasecmp(an, "expires") && av) {
            struct tm tm;
            memset(&tm, 0, sizeof tm);
            /*
             * The common forms: "Wdy, DD Mon YYYY HH:MM:SS GMT" and the
             * dash-separated variant. `timegm` is used because the value is
             * always GMT, and the *parse* decides validity — the epoch itself is
             * 0, which is a legitimate (expired) value and must not read as
             * "unparsed".
             */
            if (strptime(av, "%a, %d %b %Y %H:%M:%S", &tm) ||
                strptime(av, "%a, %d-%b-%Y %H:%M:%S", &tm)) {
                expires = (long)timegm(&tm);
                /* An epoch-0 parse is a real expiry, but the caller's "0 means
                 * session" sentinel would collide, so an already-past time is
                 * marked as 1 (the same as an expired Max-Age). */
                if (expires == 0) expires = 1;
            }
        } else if (!strcasecmp(an, "secure")) {
            secure = 1;
        } else if (!strcasecmp(an, "httponly")) {
            http_only = 1;
        }
        if (!next) break;
        attr = next + 1;
    }

    /* A host-only cookie takes the request host. A Domain cookie must cover the
     * host, or a site could set a cookie for someone else's domain. */
    if (!domain) {
        domain = request_host ? mi_strdup(request_host) : mi_strdup("");
    } else if (request_host && !domain_matches(request_host, domain)) {
        free(domain);
        free(path);
        free(line);
        return;
    }
    if (!path) path = default_path(request_path);

    /* Replace a cookie with the same name/domain/path, which is how a login
     * rotates a session id. */
    pthread_mutex_lock(&jar->lock);
    size_t slot = (size_t)-1;
    for (size_t i = 0; i < jar->count; i++) {
        if (!strcmp(jar->cookies[i].name, name) &&
            !strcasecmp(jar->cookies[i].domain, domain) &&
            !strcmp(jar->cookies[i].path, path)) {
            slot = i;
            break;
        }
    }
    if (slot != (size_t)-1) {
        free(jar->cookies[slot].value);
        free(jar->cookies[slot].domain);
        free(jar->cookies[slot].path);
        memmove(&jar->cookies[slot], &jar->cookies[slot + 1],
                (jar->count - slot - 1) * sizeof jar->cookies[0]);
        jar->count--;
    }
    /* An expiry in the past is a deletion, which the removal above has done. */
    int expired = expires > 0 && expires <= (long)time(NULL);
    if (!expired) {
        if (jar->count == jar->cap) {
            size_t want = jar->cap ? jar->cap * 2 : 16;
            mi_cookie *g = mi_realloc(jar->cookies, want * sizeof *g);
            if (!g) { pthread_mutex_unlock(&jar->lock); free(domain); free(path); free(line); return; }
            jar->cookies = g;
            jar->cap = want;
        }
        mi_cookie *c = &jar->cookies[jar->count++];
        c->name = mi_strdup(name);
        c->value = mi_strdup(value);
        c->domain = domain;
        c->path = path;
        c->expires = expires;
        c->secure = secure;
        c->http_only = http_only;
    } else {
        free(domain);
        free(path);
    }
    pthread_mutex_unlock(&jar->lock);
    free(line);
}

void mi_cookiejar_set_from_headers(mi_cookiejar *jar, const char *headers,
                                   const char *request_host, const char *request_path) {
    if (!jar || !headers) return;
    const char *p = headers;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n > 11 && !strncasecmp(p, "Set-Cookie:", 11)) {
            const char *v = p + 11;
            while (*v == ' ' || *v == '\t') v++;
            char *line = mi_strndup(v, n - (size_t)(v - p));
            if (line) {
                mi_cookiejar_set(jar, line, request_host, request_path);
                free(line);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/* The cookies that apply, most-specific path first (RFC 6265 §5.4). */
static int cmp_path_len_desc(const void *a, const void *b) {
    const mi_cookie *ca = a, *cb = b;
    size_t la = strlen(ca->path), lb = strlen(cb->path);
    if (la != lb) return la > lb ? -1 : 1;
    return 0;
}

char *mi_cookiejar_header_for_url(mi_cookiejar *jar, const char *url) {
    if (!jar || !url) return NULL;
    char *host = url_host(url);
    char *path = url_path(url);
    int https = !strncasecmp(url, "https://", 8);
    if (!host || !path) { free(host); free(path); return NULL; }

    pthread_mutex_lock(&jar->lock);
    /* Collect the matching cookies. */
    mi_cookie *match = mi_calloc(jar->count ? jar->count : 1, sizeof *match);
    size_t n = 0;
    long now = (long)time(NULL);
    for (size_t i = 0; i < jar->count; i++) {
        mi_cookie *c = &jar->cookies[i];
        if (c->expires > 0 && c->expires <= now) continue;
        if (!domain_matches(host, c->domain)) continue;
        if (!path_matches(path, c->path)) continue;
        if (c->secure && !https) continue;
        match[n++] = *c;
    }
    if (n > 1) qsort(match, n, sizeof *match, cmp_path_len_desc);

    mi_json_buf b;
    mi_json_init(&b);
    for (size_t i = 0; i < n; i++) {
        if (i) mi_json_raw(&b, "; ");
        mi_json_raw(&b, match[i].name);
        mi_json_raw(&b, "=");
        mi_json_raw(&b, match[i].value);
    }
    pthread_mutex_unlock(&jar->lock);
    free(match);
    free(host);
    free(path);

    char *out = *mi_json_data(&b) ? mi_strdup(mi_json_data(&b)) : NULL;
    mi_json_free(&b);
    return out;
}

size_t mi_cookiejar_count(const mi_cookiejar *jar) {
    return jar ? jar->count : 0;
}

int mi_cookiejar_has(const mi_cookiejar *jar, const char *host, const char *name) {
    if (!jar || !name) return 0;
    for (size_t i = 0; i < jar->count; i++) {
        if (!strcmp(jar->cookies[i].name, name) &&
            (!host || domain_matches(host, jar->cookies[i].domain)))
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Netscape format                                                     */
/* ------------------------------------------------------------------ */

void mi_cookiejar_load_netscape(mi_cookiejar *jar, const char *text) {
    if (!jar || !text) return;
    char *copy = mi_strdup(text);
    if (!copy) return;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#' || !*line) continue;
        char *fields[7] = {0};
        int nf = 0;
        char *fsave = NULL;
        for (char *t = strtok_r(line, "\t", &fsave); t && nf < 7;
             t = strtok_r(NULL, "\t", &fsave))
            fields[nf++] = t;
        if (nf < 7) continue;
        /* domain  include-subdomains  path  secure  expiry  name  value */
        char set[1024];
        snprintf(set, sizeof set, "%s=%s; Domain=%s; Path=%s%s", fields[5], fields[6],
                 fields[0][0] == '.' ? fields[0] + 1 : fields[0], fields[2],
                 !strcasecmp(fields[3], "TRUE") ? "; Secure" : "");
        mi_cookiejar_set(jar, set, NULL, fields[2]);
    }
    free(copy);
}

char *mi_cookiejar_to_netscape(const mi_cookiejar *jar) {
    mi_json_buf b;
    mi_json_init(&b);
    mi_json_raw(&b, "# Netscape HTTP Cookie File\n");
    for (size_t i = 0; i < jar->count; i++) {
        const mi_cookie *c = &jar->cookies[i];
        mi_json_raw(&b, c->domain);
        mi_json_raw(&b, "\tTRUE\t");
        mi_json_raw(&b, c->path && *c->path ? c->path : "/");
        mi_json_raw(&b, "\t");
        mi_json_raw(&b, c->secure ? "TRUE" : "FALSE");
        char exp[32];
        snprintf(exp, sizeof exp, "\t%ld\t", c->expires);
        mi_json_raw(&b, exp);
        mi_json_raw(&b, c->name);
        mi_json_raw(&b, "\t");
        mi_json_raw(&b, c->value);
        mi_json_raw(&b, "\n");
    }
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}
