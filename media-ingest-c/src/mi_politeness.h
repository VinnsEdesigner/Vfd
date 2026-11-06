/* mi_politeness.h - robots.txt, and a pace per host.
 *
 * A crawler without these is a liability: it ignores what a site asked of it and
 * hits as fast as the network allows. Both are per-host, so they live together.
 *
 *   robots   parse the rules a host published, and answer whether a URL may be
 *            fetched. The common directives are honoured; a rule a crawler
 *            cannot parse is treated as a refusal, because guessing "allow" is
 *            the wrong way to be wrong.
 *   pace     a minimum interval between requests to one host, from the host's
 *            own Crawl-delay when it gave one, else the caller's default. This
 *            is the governor that stops a fast loop from turning a soft block
 *            into a hard one.
 *
 * Fetching robots.txt is the caller's job — it goes through the same fetch path
 * as everything else, with the same identity and proxy — and the text is handed
 * here to parse. That keeps this module pure and testable.
 */
#ifndef MI_POLITENESS_H
#define MI_POLITENESS_H

#include <stddef.h>

typedef struct mi_politeness mi_politeness;

/* Whether the caller is a crawler that should honour robots at all. */
typedef enum {
    MI_ROBOTS_OBEY = 0,   /* honour the rules */
    MI_ROBOTS_IGNORE,     /* a user's explicit fetch of one URL: rules do not apply */
} mi_robots_policy;

mi_politeness *mi_politeness_new(void);
void mi_politeness_free(mi_politeness *p);

/*
 * Parse `robots_text` for `host` and remember the rules.
 *
 * `user_agent` is the token to match a User-agent group against; the rules for
 * the most specific matching group win, with `*` as the fallback. An empty or
 * NULL text means the host published no rules, which allows everything.
 */
void mi_politeness_set_robots(mi_politeness *p, const char *host,
                              const char *robots_text, const char *user_agent);

/*
 * Whether `url` may be fetched under the rules for `host`.
 *
 * Returns 1 to allow, 0 to refuse. A host with no rules allows everything; a
 * host whose robots.txt could not be read (a 5xx) is *allowed*, which is the
 * convention — a broken robots.txt must not take a site off the web.
 */
int mi_politeness_allowed(const mi_politeness *p, const char *host, const char *url);

/* Whether robots have been set for `host` (so the caller can fetch them once). */
int mi_politeness_have_robots(const mi_politeness *p, const char *host);

/*
 * How many seconds to wait before the next request to `host`.
 *
 * 0 means go now. The caller sleeps this long, then calls
 * `mi_politeness_note_request` when it actually sends.
 */
double mi_politeness_delay(const mi_politeness *p, const char *host);

/* Record that a request to `host` was just sent, starting its interval. */
void mi_politeness_note_request(mi_politeness *p, const char *host);

/*
 * The default minimum interval, in seconds, for a host that published no
 * Crawl-delay. 0 disables pacing. Set once at start-up.
 */
void mi_politeness_set_default_interval(mi_politeness *p, double seconds);

/* The interval in force for `host`: its Crawl-delay, or the default. */
double mi_politeness_interval(const mi_politeness *p, const char *host);

/*
 * The `Sitemap:` directives a host's robots.txt carried, as an owned
 * NULL-terminated array the caller frees with `mi_politeness_free_sitemaps`.
 *
 * A robots.txt is where a site declares its sitemaps, so a crawler that already
 * reads robots.txt for the rules should also pick these up — the sitemap is the
 * site's own index of what exists, and seeding a crawl from it is far better
 * coverage than hoping a link reaches every page.
 *
 * Returns NULL when the host published none.
 */
char **mi_politeness_sitemaps(const mi_politeness *p, const char *host);

/* Free a list returned by `mi_politeness_sitemaps`. */
void mi_politeness_free_sitemaps(char **sitemaps);

#endif
