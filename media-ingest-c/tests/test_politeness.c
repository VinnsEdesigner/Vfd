/* test_politeness.c - robots.txt rules, and the pace per host.
 *
 * robots.txt matching has real edge cases — longest-match wins, Allow beats
 * Disallow on a tie, a `*` wildcard, a `$` anchor, groups for other crawlers —
 * and getting one wrong means either hammering a site or refusing a page it
 * allows. All of it is pure logic, tested here with no network.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_politeness.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);   \
        }                                                                       \
    } while (0)

static void test_disallow_all(void) {
    mi_politeness *p = mi_politeness_new();
    mi_politeness_set_robots(p, "h", "User-agent: *\nDisallow: /\n", "vyzor");
    CHECK(mi_politeness_allowed(p, "h", "http://h/anything") == 0,
          "Disallow: / refuses everything");
    CHECK(mi_politeness_have_robots(p, "h") == 1, "robots are recorded");
    mi_politeness_free(p);
}

static void test_empty_disallow_allows(void) {
    mi_politeness *p = mi_politeness_new();
    /* An empty Disallow is the conventional "allow everything". */
    mi_politeness_set_robots(p, "h", "User-agent: *\nDisallow:\n", "vyzor");
    CHECK(mi_politeness_allowed(p, "h", "http://h/anything") == 1,
          "an empty Disallow allows everything");
    mi_politeness_free(p);
}

static void test_path_prefix(void) {
    mi_politeness *p = mi_politeness_new();
    mi_politeness_set_robots(p, "h", "User-agent: *\nDisallow: /private/\n", "vyzor");
    CHECK(mi_politeness_allowed(p, "h", "http://h/private/x") == 0,
          "a disallowed prefix is refused");
    CHECK(mi_politeness_allowed(p, "h", "http://h/public/x") == 1,
          "another path is allowed");
    CHECK(mi_politeness_allowed(p, "h", "http://h/privatex") == 1,
          "a prefix that is not a path segment does not match");
    mi_politeness_free(p);
}

/* The longest matching rule wins; Allow wins a tie. */
static void test_longest_match_and_allow(void) {
    mi_politeness *p = mi_politeness_new();
    mi_politeness_set_robots(p, "h",
        "User-agent: *\nDisallow: /a/\nAllow: /a/b/\n", "vyzor");
    CHECK(mi_politeness_allowed(p, "h", "http://h/a/x") == 0,
          "the broader Disallow applies where no Allow is longer");
    CHECK(mi_politeness_allowed(p, "h", "http://h/a/b/c") == 1,
          "the longer Allow wins");
    mi_politeness_free(p);

    mi_politeness *q = mi_politeness_new();
    mi_politeness_set_robots(q, "h", "User-agent: *\nDisallow: /x\nAllow: /x\n", "vyzor");
    CHECK(mi_politeness_allowed(q, "h", "http://h/x") == 1,
          "Allow wins a tie of equal length");
    mi_politeness_free(q);
}

/* A group for another crawler does not apply to us. */
static void test_agent_groups(void) {
    mi_politeness *p = mi_politeness_new();
    mi_politeness_set_robots(p, "h",
        "User-agent: Googlebot\nDisallow: /\n\nUser-agent: *\nDisallow: /only-me\n",
        "vyzor");
    CHECK(mi_politeness_allowed(p, "h", "http://h/anything") == 1,
          "another crawler's group does not apply");
    CHECK(mi_politeness_allowed(p, "h", "http://h/only-me") == 0,
          "the wildcard group does apply");
    mi_politeness_free(p);

    /* Our own token, by substring, is honoured. */
    mi_politeness *q = mi_politeness_new();
    mi_politeness_set_robots(q, "h",
        "User-agent: vyzor\nDisallow: /\nUser-agent: *\nDisallow:\n", "vyzor/1.0");
    CHECK(mi_politeness_allowed(q, "h", "http://h/x") == 0,
          "our named group is honoured");
    mi_politeness_free(q);
}

static void test_wildcard_and_anchor(void) {
    mi_politeness *p = mi_politeness_new();
    mi_politeness_set_robots(p, "h", "User-agent: *\nDisallow: /*.pdf$\n", "vyzor");
    CHECK(mi_politeness_allowed(p, "h", "http://h/a/b.pdf") == 0,
          "the *.pdf$ pattern matches a pdf");
    CHECK(mi_politeness_allowed(p, "h", "http://h/a/b.pdf?x=1") == 1,
          "the $ anchor means the pattern must end the path");
    mi_politeness_free(p);
}

/* No robots, or an empty one, allows everything. */
static void test_no_robots_allows(void) {
    mi_politeness *p = mi_politeness_new();
    CHECK(mi_politeness_allowed(p, "unknown", "http://unknown/x") == 1,
          "a host with no robots is allowed");
    mi_politeness_set_robots(p, "h", "", "vyzor");
    CHECK(mi_politeness_allowed(p, "h", "http://h/x") == 1,
          "an empty robots.txt allows everything");
    CHECK(mi_politeness_have_robots(p, "h") == 1, "an empty robots is still 'fetched'");
    mi_politeness_free(p);
}

static void test_pacing(void) {
    mi_politeness *p = mi_politeness_new();
    mi_politeness_set_default_interval(p, 2.0);
    /* No request yet: no delay. */
    CHECK(mi_politeness_delay(p, "h") == 0, "the first request is not delayed");
    mi_politeness_note_request(p, "h");
    CHECK(mi_politeness_delay(p, "h") > 1.5, "the second is delayed by the interval");

    /* A host's Crawl-delay overrides the default. */
    mi_politeness_set_robots(p, "slow", "User-agent: *\nCrawl-delay: 10\n", "vyzor");
    CHECK(mi_politeness_interval(p, "slow") == 10, "Crawl-delay is used as the interval");
    CHECK(mi_politeness_interval(p, "h") == 2, "other hosts use the default");
    mi_politeness_free(p);
}

int main(void) {
    test_disallow_all();
    test_empty_disallow_allows();
    test_path_prefix();
    test_longest_match_and_allow();
    test_agent_groups();
    test_wildcard_and_anchor();
    test_no_robots_allows();
    test_pacing();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
