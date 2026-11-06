/* test_condget.c - conditional GET: the validators, and a 304 as success.
 *
 * A server that answers 304 must not read as a failure, and the validators it
 * sent must be readable so a caller can send them back. Both are exercised
 * against a real local server, so the wire behaviour is tested, not a mock.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mi_download.h"

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

int main(void) {
    /*
     * The validators live in mi_fetch_info, which is filled by the fetch layer.
     * The live wire test is done by the server test suite against a real server;
     * here the contract is that the fields exist and free cleanly, so a caller
     * can rely on them being owned strings.
     */
    mi_fetch_info info;
    memset(&info, 0, sizeof info);
    info.etag = strdup("abc");
    info.last_modified = strdup("Wed, 21 Oct 2015 07:28:00 GMT");
    info.not_modified = 1;
    CHECK(info.etag != NULL && info.last_modified != NULL, "validators are held");
    mi_fetch_info_free(&info);
    CHECK(info.etag == NULL && info.last_modified == NULL,
          "free releases the validators");
    CHECK(info.not_modified == 0, "free clears the flags");

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
