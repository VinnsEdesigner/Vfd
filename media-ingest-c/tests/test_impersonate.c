/* test_impersonate.c - finding the impersonation helper.
 *
 * The network call is the helper's job and is exercised live, not here; what
 * this tests is the part the engine owns: that it finds the helper the operator
 * named, and reports honestly when there is none, so a caller can fall back
 * rather than fail a fetch that libcurl could still do.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mi_impersonate.h"

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
    mi_impersonate_config cfg;
    memset(&cfg, 0, sizeof cfg);

    /* A helper path that exists and is executable is found. */
    char path[64];
    snprintf(path, sizeof path, "/tmp/mi-imp-test-%d.sh", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs("#!/bin/sh\nexit 0\n", f);
        fclose(f);
        chmod(path, 0755);
    }
    cfg.helper_path = path;
    char *resolved = NULL;
    CHECK(mi_impersonate_available(&cfg, &resolved) == 1, "an executable helper is found");
    CHECK(resolved != NULL, "the resolved path is returned");
    free(resolved);
    resolved = NULL;

    /* A helper path that does not exist is reported missing, not guessed at. */
    cfg.helper_path = "/nonexistent/fetch-impersonate-xyz";
    CHECK(mi_impersonate_available(&cfg, &resolved) == 0,
          "a named helper that is missing is reported missing");
    CHECK(resolved == NULL, "no path is returned for a missing helper");

    /* A fetch with no helper fails cleanly with a reason, not a crash. */
    char err[160];
    char *body = mi_impersonate_fetch(&cfg, "http://example.com/", 0, err, sizeof err);
    CHECK(body == NULL, "a fetch with no helper returns NULL");
    CHECK(strstr(err, "helper") != NULL, "the failure names the missing helper");

    unlink(path);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
