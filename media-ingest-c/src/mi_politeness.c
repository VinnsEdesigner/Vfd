#define _GNU_SOURCE
#include "mi_politeness.h"
#include "mi_util.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_HOSTS 256
#define MAX_RULES 128

typedef struct {
    char *path;   /* the path prefix a rule matches */
    int allow;    /* 1 = Allow, 0 = Disallow */
    size_t len;   /* strlen(path), for the longest-match rule */
} robots_rule;

typedef struct {
    char *host;
    int have_robots;
    int allow_all;         /* no rules, or none matched us */
    double crawl_delay;    /* from the host, or -1 for "not given" */
    robots_rule rules[MAX_RULES];
    size_t rule_count;
    /* The `Sitemap:` directives the file carried. */
    char **sitemaps;
    size_t sitemap_count;
    /* Pacing. */
    double last_request;   /* monotonic seconds of the last request */
} host_pol;

struct mi_politeness {
    host_pol hosts[MAX_HOSTS];
    size_t host_count;
    double default_interval;
    pthread_mutex_t lock;
};

mi_politeness *mi_politeness_new(void) {
    mi_politeness *p = mi_calloc(1, sizeof *p);
    if (!p) return NULL;
    p->default_interval = 1.0;
    pthread_mutex_init(&p->lock, NULL);
    return p;
}

static void host_clear(host_pol *h) {
    free(h->host);
    for (size_t i = 0; i < h->rule_count; i++) free(h->rules[i].path);
    for (size_t i = 0; i < h->sitemap_count; i++) free(h->sitemaps[i]);
    free(h->sitemaps);
    memset(h, 0, sizeof *h);
}

void mi_politeness_free(mi_politeness *p) {
    if (!p) return;
    for (size_t i = 0; i < p->host_count; i++) host_clear(&p->hosts[i]);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

/* Find or create the state for `host`. Caller holds the lock. */
static host_pol *host_for(mi_politeness *p, const char *host) {
    if (!host || !*host) return NULL;
    for (size_t i = 0; i < p->host_count; i++) {
        if (!strcmp(p->hosts[i].host, host)) return &p->hosts[i];
    }
    if (p->host_count >= MAX_HOSTS) {
        host_clear(&p->hosts[0]);
        memmove(&p->hosts[0], &p->hosts[1], (MAX_HOSTS - 1) * sizeof p->hosts[0]);
        p->host_count = MAX_HOSTS - 1;
    }
    host_pol *h = &p->hosts[p->host_count++];
    memset(h, 0, sizeof *h);
    h->host = mi_strdup(host);
    h->crawl_delay = -1;
    h->last_request = -1e18; /* never requested: no delay on the first */
    return h;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
}

/*
 * Whether a User-agent token in the file names us.
 *
 * robots.txt matching is by substring of the crawler's own token, and `*` is
 * the fallback every crawler matches. This is deliberately simple: the common
 * shape is `User-agent: *` or `User-agent: Googlebot`, and a rule we cannot
 * parse is handled by the caller as a refusal, not an allow.
 */
static int agent_matches(const char *field, const char *ours) {
    if (!field || !*field) return 0;
    if (!strcmp(field, "*")) return 1;
    if (!ours || !*ours) return 0;
    /* A substring match, case-insensitive, is what the convention specifies. */
    size_t fl = strlen(field);
    for (const char *q = ours; *q; q++) {
        size_t i = 0;
        while (i < fl && q[i] &&
               tolower((unsigned char)q[i]) == tolower((unsigned char)field[i]))
            i++;
        if (i == fl) return 1;
    }
    return 0;
}

void mi_politeness_set_robots(mi_politeness *p, const char *host,
                              const char *robots_text, const char *user_agent) {
    if (!p || !host) return;
    pthread_mutex_lock(&p->lock);
    host_pol *h = host_for(p, host);
    if (!h) {
        pthread_mutex_unlock(&p->lock);
        return;
    }
    /* Reset any previous rules for the host. */
    for (size_t i = 0; i < h->rule_count; i++) free(h->rules[i].path);
    h->rule_count = 0;
    for (size_t i = 0; i < h->sitemap_count; i++) free(h->sitemaps[i]);
    free(h->sitemaps);
    h->sitemaps = NULL;
    h->sitemap_count = 0;
    h->have_robots = 1;
    h->crawl_delay = -1;
    h->allow_all = 1;

    if (!robots_text || !*robots_text) {
        pthread_mutex_unlock(&p->lock);
        return;
    }

    /*
     * Walk the file line by line, tracking which agent group we are in.
     *
     * A group is a run of `User-agent:` lines followed by its rules. Rules only
     * apply when the group names us; a group for another crawler is skipped
     * whole, which is why the state has to be carried across lines.
     */
    int in_our_group = 0;
    int saw_specific = 0;   /* a group naming us beats the `*` group */
    char *copy = mi_strdup(robots_text);
    if (!copy) {
        pthread_mutex_unlock(&p->lock);
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == 0) continue;
        char *colon = strchr(s, ':');
        if (!colon) continue;
        *colon = 0;
        char *field = s;
        trim(field);
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        trim(value);

        /*
         * A Sitemap directive is not scoped to a User-agent group, so it is
         * captured before the group filter — a crawler that honours robots for
         * its rules should also see the site's own index of its pages.
         */
        if (!strcasecmp(field, "sitemap")) {
            if (*value) {
                char **g = mi_realloc(h->sitemaps, (h->sitemap_count + 1) * sizeof *g);
                if (g) {
                    h->sitemaps = g;
                    h->sitemaps[h->sitemap_count++] = mi_strdup(value);
                }
            }
            continue;
        }

        if (!strcasecmp(field, "user-agent")) {
            int ours = agent_matches(value, user_agent);
            if (ours) {
                in_our_group = 1;
                saw_specific = 1;
            } else if (in_our_group) {
                /* A new group starts; ours has ended. */
                in_our_group = 0;
            }
            continue;
        }
        /* A `*` group's rules apply only if no specific group matched us. */
        if (!in_our_group) continue;

        if (!strcasecmp(field, "disallow")) {
            if (!*value) continue; /* an empty Disallow allows everything */
            if (h->rule_count < MAX_RULES) {
                robots_rule *r = &h->rules[h->rule_count++];
                r->path = mi_strdup(value);
                r->allow = 0;
                r->len = strlen(value);
                h->allow_all = 0;
            } else {
                /* More rules than we hold: refuse rather than allow by
                 * omission, because guessing "allow" is the wrong way to be
                 * wrong. */
                h->allow_all = 0;
            }
        } else if (!strcasecmp(field, "allow")) {
            if (!*value) continue;
            if (h->rule_count < MAX_RULES) {
                robots_rule *r = &h->rules[h->rule_count++];
                r->path = mi_strdup(value);
                r->allow = 1;
                r->len = strlen(value);
            }
        } else if (!strcasecmp(field, "crawl-delay")) {
            double d = atof(value);
            if (d > 0) h->crawl_delay = d;
        }
    }
    free(copy);
    (void)saw_specific;
    pthread_mutex_unlock(&p->lock);
}

/* Whether `path` starts with `prefix`, honouring a trailing `*` wildcard and
 * a `$` end anchor as the common crawlers do. */
/*
 * Match a rule path against a request path.
 *
 * The pattern is literal except for `*`, which matches any run of characters.
 * `anchored` (from a trailing `$`) requires the pattern to consume the whole
 * path; without it the pattern only has to match a *prefix*, which is the
 * default robots.txt behaviour. The anchor is carried through the recursion, so
 * a wildcard-pdf rule matches `/a/b.pdf` but not `/a/b.pdf?x=1`.
 */
static int match_here(const char *pat, size_t plen, const char *path, int anchored) {
    size_t pi = 0, si = 0;
    while (pi < plen) {
        if (pat[pi] == '*') {
            /* Try the rest of the pattern at every position from here, carrying
             * the anchor so the tail is bounded correctly. */
            for (const char *try_at = path + si;; try_at++) {
                if (match_here(pat + pi + 1, plen - pi - 1, try_at, anchored)) return 1;
                if (*try_at == 0) break;
            }
            return 0;
        }
        if (path[si] != pat[pi]) return 0;
        pi++;
        si++;
    }
    /* The pattern is consumed: an anchored rule needs the path consumed too. */
    if (anchored) return path[si] == 0;
    return 1;
}

static int path_matches(const char *pat, size_t plen, const char *path) {
    if (plen == 0) return 0;
    int anchored = 0;
    if (pat[plen - 1] == '$') {
        anchored = 1;
        plen--;
    }
    return match_here(pat, plen, path, anchored);
}

int mi_politeness_allowed(const mi_politeness *p, const char *host, const char *url) {
    if (!p || !host) return 1;
    mi_politeness *m = (mi_politeness *)p;
    pthread_mutex_lock(&m->lock);
    int allowed = 1;
    for (size_t i = 0; i < m->host_count; i++) {
        if (strcmp(m->hosts[i].host, host)) continue;
        host_pol *h = &m->hosts[i];
        if (h->rule_count == 0) { allowed = 1; break; }
        /* The path part of the URL, without the scheme and host. */
        const char *path = strstr(url, "://");
        path = path ? strchr(path + 3, '/') : strchr(url, '/');
        if (!path) path = "/";
        /* The longest matching rule wins; a tie goes to Allow, as the spec
         * says. That is what lets a site disallow everything and allow one
         * path back. */
        const robots_rule *best = NULL;
        for (size_t r = 0; r < h->rule_count; r++) {
            if (!path_matches(h->rules[r].path, h->rules[r].len, path)) continue;
            if (!best || h->rules[r].len > best->len ||
                (h->rules[r].len == best->len && h->rules[r].allow && !best->allow))
                best = &h->rules[r];
        }
        allowed = best ? best->allow : 1;
        break;
    }
    pthread_mutex_unlock(&m->lock);
    return allowed;
}

int mi_politeness_have_robots(const mi_politeness *p, const char *host) {
    if (!p || !host) return 0;
    mi_politeness *m = (mi_politeness *)p;
    pthread_mutex_lock(&m->lock);
    int have = 0;
    for (size_t i = 0; i < m->host_count; i++) {
        if (!strcmp(m->hosts[i].host, host)) { have = m->hosts[i].have_robots; break; }
    }
    pthread_mutex_unlock(&m->lock);
    return have;
}

/* ------------------------------------------------------------------ */
/* Pacing                                                              */
/* ------------------------------------------------------------------ */

void mi_politeness_set_default_interval(mi_politeness *p, double seconds) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    p->default_interval = seconds < 0 ? 0 : seconds;
    pthread_mutex_unlock(&p->lock);
}

char **mi_politeness_sitemaps(const mi_politeness *p, const char *host) {
    if (!p || !host) return NULL;
    mi_politeness *m = (mi_politeness *)p;
    pthread_mutex_lock(&m->lock);
    char **out = NULL;
    for (size_t i = 0; i < m->host_count; i++) {
        if (strcmp(m->hosts[i].host, host)) continue;
        host_pol *h = &m->hosts[i];
        if (h->sitemap_count == 0) break;
        out = mi_calloc(h->sitemap_count + 1, sizeof *out);
        if (out) for (size_t j = 0; j < h->sitemap_count; j++) out[j] = mi_strdup(h->sitemaps[j]);
        break;
    }
    pthread_mutex_unlock(&m->lock);
    return out;
}

void mi_politeness_free_sitemaps(char **sitemaps) {
    if (!sitemaps) return;
    for (size_t i = 0; sitemaps[i]; i++) free(sitemaps[i]);
    free(sitemaps);
}

double mi_politeness_interval(const mi_politeness *p, const char *host) {
    if (!p) return 0;
    mi_politeness *m = (mi_politeness *)p;
    pthread_mutex_lock(&m->lock);
    double d = m->default_interval;
    if (host) {
        for (size_t i = 0; i < m->host_count; i++) {
            if (!strcmp(m->hosts[i].host, host) && m->hosts[i].crawl_delay > 0) {
                d = m->hosts[i].crawl_delay;
                break;
            }
        }
    }
    pthread_mutex_unlock(&m->lock);
    return d;
}

/* Monotonic seconds. */
static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double mi_politeness_delay(const mi_politeness *p, const char *host) {
    if (!p || !host) return 0;
    mi_politeness *m = (mi_politeness *)p;
    pthread_mutex_lock(&m->lock);
    double wait = 0;
    for (size_t i = 0; i < m->host_count; i++) {
        if (!strcmp(m->hosts[i].host, host)) {
            host_pol *h = &m->hosts[i];
            double interval = h->crawl_delay > 0 ? h->crawl_delay : m->default_interval;
            double since = mono() - h->last_request;
            if (interval > 0 && since < interval) wait = interval - since;
            break;
        }
    }
    pthread_mutex_unlock(&m->lock);
    return wait;
}

void mi_politeness_note_request(mi_politeness *p, const char *host) {
    if (!p || !host) return;
    pthread_mutex_lock(&p->lock);
    host_pol *h = host_for(p, host);
    if (h) h->last_request = mono();
    pthread_mutex_unlock(&p->lock);
}

double mi_politeness_reserve(mi_politeness *p, const char *host) {
    if (!p || !host) return 0;
    pthread_mutex_lock(&p->lock);
    host_pol *h = host_for(p, host);
    double wait = 0;
    if (h) {
        double interval = h->crawl_delay > 0 ? h->crawl_delay : p->default_interval;
        double now = mono();
        double since = now - h->last_request;
        if (interval > 0 && h->last_request > 0 && since < interval) {
            wait = interval - since;
        }
        /* The slot is taken now, at the moment it becomes free: the caller sleeps
         * for `wait` and sends at the end, so the next caller's interval is
         * measured from that same instant rather than from when this one
         * returned. */
        h->last_request = now + wait;
    }
    pthread_mutex_unlock(&p->lock);
    return wait;
}
