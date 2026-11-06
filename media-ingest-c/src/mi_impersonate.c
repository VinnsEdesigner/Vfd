#define _GNU_SOURCE
#include "mi_impersonate.h"
#include "mi_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The venv the toolchain installs into, relative to the workspace, and the
 * helper beside it. A service started by systemd has neither on PATH, so the
 * search covers the common install locations rather than trusting PATH alone. */
static const char *CANDIDATES[] = {
    "fetch-impersonate",
    "scripts/fetch-impersonate.py",
    "media-ingest-c/scripts/fetch-impersonate.py",
    "scripts/fetch-impersonate",
    "./scripts/fetch-impersonate.py",
    NULL,
};

/* Whether a path names a runnable file. */
static int is_executable(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) && (st.st_mode & 0111) != 0;
}

/* Whether a path ends in ".py" (so it must be run through python3). */
static int is_python(const char *path) {
    size_t n = strlen(path);
    return n >= 3 && !strcmp(path + n - 3, ".py");
}

int mi_impersonate_available(const mi_impersonate_config *cfg, char **resolved) {
    if (resolved) *resolved = NULL;
    const char *env = getenv("MI_IMPERSONATE_HELPER");
    if (cfg && cfg->helper_path && *cfg->helper_path) {
        if (mi_which(cfg->helper_path) || is_executable(cfg->helper_path)) {
            if (resolved) *resolved = mi_strdup(cfg->helper_path);
            return 1;
        }
        return 0;
    }
    if (env && *env && (mi_which(env) || is_executable(env))) {
        if (resolved) *resolved = mi_strdup(env);
        return 1;
    }
    for (size_t i = 0; CANDIDATES[i]; i++) {
        /* A repo-relative helper is not on PATH, so the path is tested directly
         * as well as searched — a service started from the workspace finds its
         * own scripts this way. */
        if (mi_which(CANDIDATES[i]) || is_executable(CANDIDATES[i])) {
            if (resolved) *resolved = mi_strdup(CANDIDATES[i]);
            return 1;
        }
    }
    return 0;
}

/* Read a file whole, up to a cap. Returns NULL on error. */
static char *read_file_capped(const char *path, size_t max_bytes, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = max_bytes ? max_bytes : 64 * 1024 * 1024;
    char *buf = mi_malloc(cap + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) *len_out = got;
    return buf;
}

char *mi_impersonate_fetch(const mi_impersonate_config *cfg, const char *url,
                           size_t max_bytes, char **meta_out, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (meta_out) *meta_out = NULL;
    if (!url) {
        if (err && errcap) snprintf(err, errcap, "no url");
        return NULL;
    }
    char *helper = NULL;
    if (!mi_impersonate_available(cfg, &helper)) {
        if (err && errcap) snprintf(err, errcap, "no impersonation helper installed");
        return NULL;
    }

    /* A private temp file for the body: the helper writes it, we read it. 0600
     * because a fetched page may be behind a session cookie. */
    char tmpl[] = "/tmp/vyzor-impersonate-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        free(helper);
        if (err && errcap) snprintf(err, errcap, "cannot make a temp file: %s", strerror(errno));
        return NULL;
    }
    close(fd);

    /* A second temp file for the response metadata, when the caller wants it. */
    char meta_tmpl[64] = {0};
    if (meta_out) {
        snprintf(meta_tmpl, sizeof meta_tmpl, "/tmp/vyzor-impersonate-meta-XXXXXX");
        int mfd = mkstemp(meta_tmpl);
        if (mfd >= 0) close(mfd);
        else meta_tmpl[0] = '\0';
    }

    /* The helper takes a cookie *file*; write the caller's text to one, 0600,
     * and remove it after. */
    char cookie_tmp[64] = {0};
    if (cfg && cfg->cookies_text && *cfg->cookies_text) {
        snprintf(cookie_tmp, sizeof cookie_tmp, "/tmp/vyzor-impersonate-ck-XXXXXX");
        int cfd = mkstemp(cookie_tmp);
        if (cfd >= 0) {
            fchmod(cfd, 0600);
            size_t cl = strlen(cfg->cookies_text);
            ssize_t w = write(cfd, cfg->cookies_text, cl);
            close(cfd);
            if (w < 0 || (size_t)w != cl) {
                unlink(cookie_tmp);
                cookie_tmp[0] = '\0';
            }
        } else {
            cookie_tmp[0] = '\0';
        }
    }

    char timeout_s[16];
    snprintf(timeout_s, sizeof timeout_s, "%d",
             (cfg && cfg->timeout_seconds > 0) ? cfg->timeout_seconds : 30);
    char max_s[32];
    snprintf(max_s, sizeof max_s, "%zu", max_bytes);

    const char *impersonate = (cfg && cfg->impersonate && *cfg->impersonate)
                                  ? cfg->impersonate : "chrome";

    /* argv, built once. A .py helper runs through python3. */
    const char *py = is_python(helper) ? "python3" : NULL;
    const char *argv[24];
    int a = 0;
    if (py) argv[a++] = py;
    argv[a++] = helper;
    argv[a++] = "--url";
    argv[a++] = url;
    argv[a++] = "--out";
    argv[a++] = tmpl;
    if (meta_tmpl[0]) {
        argv[a++] = "--meta-out";
        argv[a++] = meta_tmpl;
    }
    argv[a++] = "--impersonate";
    argv[a++] = impersonate;
    argv[a++] = "--timeout";
    argv[a++] = timeout_s;
    if (max_bytes) {
        argv[a++] = "--max-bytes";
        argv[a++] = max_s;
    }
    if (cfg && cfg->proxy && *cfg->proxy) {
        argv[a++] = "--proxy";
        argv[a++] = cfg->proxy;
    }
    if (cfg && cfg->user_agent && *cfg->user_agent) {
        argv[a++] = "--user-agent";
        argv[a++] = cfg->user_agent;
    }
    if (cookie_tmp[0]) {
        argv[a++] = "--cookies-file";
        argv[a++] = cookie_tmp;
    }
    argv[a] = NULL;

    /* Capture the helper's stderr so its one-line reason becomes ours. */
    int errpipe[2];
    if (pipe(errpipe) != 0) {
        unlink(tmpl);
        if (meta_tmpl[0]) unlink(meta_tmpl);
        if (cookie_tmp[0]) unlink(cookie_tmp);
        free(helper);
        if (err && errcap) snprintf(err, errcap, "pipe failed");
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        unlink(tmpl);
        if (meta_tmpl[0]) unlink(meta_tmpl);
        if (cookie_tmp[0]) unlink(cookie_tmp);
        free(helper);
        if (err && errcap) snprintf(err, errcap, "fork failed");
        return NULL;
    }
    if (pid == 0) {
        dup2(errpipe[1], STDERR_FILENO);
        close(errpipe[0]);
        close(errpipe[1]);
        /* Resolve through PATH the same way a shell would. */
        if (py) execvp(py, (char *const *)argv);
        else execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(errpipe[1]);

    int status = 0;
    waitpid(pid, &status, 0);

    /* Read the helper's stderr (bounded). */
    char stderr_buf[512];
    size_t sn = 0;
    for (;;) {
        ssize_t r = read(errpipe[0], stderr_buf + sn, sizeof stderr_buf - 1 - sn);
        if (r <= 0) break;
        sn += (size_t)r;
        if (sn >= sizeof stderr_buf - 1) break;
    }
    stderr_buf[sn] = '\0';
    close(errpipe[0]);

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code != 0) {
        if (err && errcap) {
            /* Prefer the helper's own reason; it is more specific than a code. */
            if (sn > 0) {
                /* Trim a trailing newline. */
                while (sn > 0 && (stderr_buf[sn - 1] == '\n' || stderr_buf[sn - 1] == '\r'))
                    stderr_buf[--sn] = '\0';
                snprintf(err, errcap, "%s", stderr_buf);
            } else {
                snprintf(err, errcap, "the impersonation helper exited %d", code);
            }
        }
        unlink(tmpl);
        if (meta_tmpl[0]) unlink(meta_tmpl);
        if (cookie_tmp[0]) unlink(cookie_tmp);
        free(helper);
        return NULL;
    }

    size_t len = 0;
    char *body = read_file_capped(tmpl, max_bytes, &len);
    unlink(tmpl);
    if (meta_tmpl[0]) {
        if (meta_out) {
            size_t mlen = 0;
            *meta_out = read_file_capped(meta_tmpl, 256 * 1024, &mlen);
        }
        unlink(meta_tmpl);
    }
    if (cookie_tmp[0]) unlink(cookie_tmp);
    free(helper);
    if (!body) {
        if (err && errcap) snprintf(err, errcap, "the helper produced no body");
        return NULL;
    }
    return body;
}
