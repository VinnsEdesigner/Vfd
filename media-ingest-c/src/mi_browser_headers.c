#include "mi_browser_headers.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * A recent Chrome, used when the caller names a family but no version. The exact
 * number matters little; that the UA, `sec-ch-ua` and the family agree matters a
 * great deal, which is why they are all derived from one value here.
 */
#define DEFAULT_CHROME_VERSION "131"

mi_browser_family mi_browser_family_of(const char *impersonate) {
    if (!impersonate || !*impersonate) return MI_BROWSER_CHROME;
    if (!strncasecmp(impersonate, "firefox", 7)) return MI_BROWSER_FIREFOX;
    if (!strncasecmp(impersonate, "safari", 6)) return MI_BROWSER_SAFARI;
    return MI_BROWSER_CHROME;
}

const char *mi_browser_default_version(void) { return DEFAULT_CHROME_VERSION; }

/* Pull the major version out of a Chrome-like UA, so sec-ch-ua can match it. */
static char *version_from_ua(const char *ua) {
    if (!ua) return NULL;
    const char *p = strstr(ua, "Chrome/");
    if (!p) p = strstr(ua, "Chromium/");
    if (!p) return NULL;
    p += strlen("Chrome/");
    const char *end = p;
    while (*end && *end != '.' && *end != ' ' && *end != ';') end++;
    if (end == p) return NULL;
    return mi_strndup(p, (size_t)(end - p));
}

size_t mi_browser_headers(mi_browser_family family, const char *version,
                          const char *user_agent, char **out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    size_t n = 0;

    /* The version: the caller's, else the UA's, else the default. */
    char *from_ua = version_from_ua(user_agent);
    const char *ver = version && *version ? version
                     : (from_ua ? from_ua : DEFAULT_CHROME_VERSION);

    /*
     * The order a browser sends these in. `Accept` first, then the encoding and
     * language, then the client hints, then the fetch metadata. A site that
     * checks order sees the same shape it sees from Chrome.
     */
    const char *const BASE[] = {
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
        "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7",
        "Accept-Encoding: gzip, deflate, br, zstd",
        "Accept-Language: en-US,en;q=0.9",
        "Cache-Control: max-age=0",
        "Upgrade-Insecure-Requests: 1",
    };
    for (size_t i = 0; i < sizeof BASE / sizeof BASE[0] && n < out_cap; i++) {
        char *h = mi_strdup(BASE[i]);
        if (h) out[n++] = h;
    }

    if (family == MI_BROWSER_CHROME) {
        /* The client hints, which must agree with the UA's major version. */
        char *chua = mi_asprintf(
            "sec-ch-ua: \"Chromium\";v=\"%s\", \"Not?A_Brand\";v=\"24\", "
            "\"Google Chrome\";v=\"%s\"",
            ver, ver);
        if (chua && n < out_cap) out[n++] = chua;
        else free(chua);
        if (n < out_cap) { char *h = mi_strdup("sec-ch-ua-mobile: ?0"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("sec-ch-ua-platform: \"Windows\""); if (h) out[n++] = h; }
        /* The fetch metadata a top-level navigation sends. */
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Dest: document"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Mode: navigate"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Site: none"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-User: ?1"); if (h) out[n++] = h; }
        /* Priority is Chromium-only too. */
        if (n < out_cap) { char *h = mi_strdup("Priority: u=0, i"); if (h) out[n++] = h; }
    } else if (family == MI_BROWSER_FIREFOX) {
        /* Firefox sends no client hints. Sending none is the coherent choice. */
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Dest: document"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Mode: navigate"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Site: none"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-User: ?1"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("TE: trailers"); if (h) out[n++] = h; }
    } else {
        /* Safari: fetch metadata, no client hints, no Priority. */
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Dest: document"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Mode: navigate"); if (h) out[n++] = h; }
        if (n < out_cap) { char *h = mi_strdup("Sec-Fetch-Site: none"); if (h) out[n++] = h; }
    }

    free(from_ua);
    return n;
}

void mi_browser_headers_free(char **headers, size_t n) {
    if (!headers) return;
    for (size_t i = 0; i < n; i++) free(headers[i]);
}
