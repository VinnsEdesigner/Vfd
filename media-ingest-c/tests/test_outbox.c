/* test_outbox.c - the bounded outbound queue, and the policy that keeps a live
 * view live.
 *
 * The behaviours worth pinning are the ones a regression would quietly break:
 * a newer picture must supersede an older one, a state change must never be
 * dropped, the byte budget must be honoured, and a full socket must not be
 * mistaken for a dead one.
 *
 * The socket is a real pipe, not a mock: `flush` is about what a real
 * non-blocking descriptor does, and a mock would only test the mock.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mi_outbox.h"
#include "mi_util.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        }                                                                   \
    } while (0)

/*
 * A socket pair with both ends non-blocking — the closest thing to a live
 * session socket. A *pipe* is not a substitute: `send()` fails on one with
 * ENOTSOCK, which is how an earlier version of this test managed to fail while
 * the code under test was correct.
 */
static void make_pipe(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        perror("socketpair");
        exit(1);
    }
    int fl = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, fl | O_NONBLOCK);
    /* The read end too, so `drain` takes what is there without waiting for
     * bytes that will never come. */
    fl = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
}

/* Read everything currently in the pipe, into a freshly allocated buffer. */
static char *drain(int fd, size_t *len_out) {
    size_t cap = 65536, len = 0;
    char *buf = mi_malloc(cap + 1);
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            buf = mi_realloc(buf, cap + 1);
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    if (len_out) *len_out = len;
    return buf;
}

static void test_basics(void) {
    mi_outbox *o = mi_outbox_new(1024);
    CHECK(o != NULL, "an outbox can be made");
    CHECK(mi_outbox_empty(o), "a fresh outbox is empty");
    CHECK(mi_outbox_bytes(o) == 0, "and holds nothing");
    CHECK(mi_outbox_count(o) == 0, "and counts nothing");
    CHECK(mi_outbox_dropped(o) == 0, "and has dropped nothing");

    CHECK(mi_outbox_push(o, "hello", 5, 0) == 0, "a message can be pushed");
    CHECK(!mi_outbox_empty(o), "and then it is not empty");
    CHECK(mi_outbox_bytes(o) == 5, "and its bytes are counted");
    CHECK(mi_outbox_count(o) == 1, "and it is counted");

    int fds[2];
    make_pipe(fds);
    CHECK(mi_outbox_flush(o, fds[1]) == 0, "a flush succeeds");
    CHECK(mi_outbox_empty(o), "and empties the queue");
    size_t len = 0;
    char *got = drain(fds[0], &len);
    CHECK(len == 5 && !memcmp(got, "hello", 5), "and the bytes arrived");
    free(got);
    close(fds[0]);
    close(fds[1]);

    mi_outbox_free(o);
    CHECK(mi_outbox_empty(NULL), "a null outbox reads as empty");
    CHECK(mi_outbox_bytes(NULL) == 0, "and has no bytes");
    mi_outbox_free(NULL); /* must not crash */
}

/*
 * The rule that keeps a live view live: a newer picture supersedes a waiting one,
 * so at most one picture is ever pending behind the one being written.
 */
static void test_drop_oldest_picture(void) {
    mi_outbox *o = mi_outbox_new(1 << 20);

    CHECK(mi_outbox_push(o, "frame-1", 7, 1) == 0, "the first picture is queued");
    CHECK(mi_outbox_count(o) == 1, "and is pending");
    CHECK(mi_outbox_push(o, "frame-2", 7, 1) == 0, "a second picture is queued");
    CHECK(mi_outbox_count(o) == 1, "but the first is dropped, not queued behind");
    CHECK(mi_outbox_dropped(o) == 1, "and the drop is counted");

    CHECK(mi_outbox_push(o, "frame-3", 7, 1) == 0, "a third picture is queued");
    CHECK(mi_outbox_count(o) == 1, "still only one pending");
    CHECK(mi_outbox_dropped(o) == 2, "and both drops are counted");

    int fds[2];
    make_pipe(fds);
    mi_outbox_flush(o, fds[1]);
    size_t len = 0;
    char *got = drain(fds[0], &len);
    /* The *newest* picture is what a viewer must see. */
    CHECK(len == 7 && !memcmp(got, "frame-3", 7), "the newest picture is the one sent");
    free(got);
    close(fds[0]);
    close(fds[1]);
    mi_outbox_free(o);
}

/* A state change carries information, so it is never dropped for a picture. */
static void test_state_is_never_dropped(void) {
    mi_outbox *o = mi_outbox_new(1 << 20);

    CHECK(mi_outbox_push(o, "state", 5, 0) == 0, "a state change is queued");
    CHECK(mi_outbox_push(o, "frame-1", 7, 1) == 0, "then a picture");
    CHECK(mi_outbox_count(o) == 2, "both are pending, because neither supersedes the other");
    CHECK(mi_outbox_dropped(o) == 0, "and nothing was dropped");

    CHECK(mi_outbox_push(o, "frame-2", 7, 1) == 0, "a newer picture arrives");
    CHECK(mi_outbox_count(o) == 2, "the picture is replaced, not added");
    CHECK(mi_outbox_dropped(o) == 1, "only the picture was dropped");

    int fds[2];
    make_pipe(fds);
    mi_outbox_flush(o, fds[1]);
    size_t len = 0;
    char *got = drain(fds[0], &len);
    CHECK(len == 12, "state plus the newest picture arrived");
    CHECK(!memcmp(got, "state", 5), "the state change came first");
    CHECK(!memcmp(got + 5, "frame-2", 7), "and the newest picture after it");
    free(got);
    close(fds[0]);
    close(fds[1]);
    mi_outbox_free(o);
}

/* The byte budget is what stops a non-reading client exhausting memory. */
static void test_byte_budget(void) {
    /* Room for two 10-byte pictures and nothing more. */
    mi_outbox *o = mi_outbox_new(20);

    CHECK(mi_outbox_push(o, "aaaaaaaaaa", 10, 1) == 0, "the first picture fits");
    CHECK(mi_outbox_push(o, "bbbbbbbbbb", 10, 1) == 0, "the second picture fits");
    /* The second replaced the first, so only one is pending. */
    CHECK(mi_outbox_count(o) == 1, "a newer picture still supersedes");
    CHECK(mi_outbox_bytes(o) == 10, "so only its bytes are held");

    /* Non-droppable messages are held to the budget by refusing more. */
    mi_outbox *big = mi_outbox_new(10);
    CHECK(mi_outbox_push(big, "1234567890", 10, 0) == 0, "a state change fills the budget");
    CHECK(mi_outbox_push(big, "12345", 5, 0) == -1, "another is refused rather than dropping it");
    CHECK(mi_outbox_count(big) == 1, "and the queue still holds the one that matters");
    mi_outbox_free(big);
    mi_outbox_free(o);
}

/* A full socket is backpressure, not a dead peer. */
static void test_full_socket_is_not_an_error(void) {
    int fds[2];
    make_pipe(fds);
    /* Shrink the pipe so it fills quickly and deterministically. */
    int sz = 4096;
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);

    mi_outbox *o = mi_outbox_new(1 << 20);
    /* Push more than the pipe can hold. */
    char big[8192];
    memset(big, 'x', sizeof big);
    for (int i = 0; i < 8; i++) {
        CHECK(mi_outbox_push(o, big, sizeof big, 0) == 0, "a large message is queued");
    }

    int rc = mi_outbox_flush(o, fds[1]);
    CHECK(rc == 0, "a full socket is not reported as a failure");
    CHECK(mi_outbox_would_block(o) > 0, "and the block is counted");
    CHECK(!mi_outbox_empty(o), "and the rest is still queued");

    /*
     * Drain and flush in a loop, which is what a real caller does: the socket
     * only accepts more as the reader takes it, so one pass is not enough.
     */
    for (int i = 0; i < 50 && !mi_outbox_empty(o); i++) {
        size_t len = 0;
        char *got = drain(fds[0], &len);
        free(got);
        CHECK(mi_outbox_flush(o, fds[1]) == 0, "the retry succeeds");
    }
    CHECK(mi_outbox_empty(o), "and clears the queue");

    close(fds[0]);
    close(fds[1]);
    mi_outbox_free(o);
}

/* A closed peer is a failure, and must be told apart from a full socket. */
static void test_closed_peer(void) {
    int fds[2];
    make_pipe(fds);
    close(fds[0]); /* the reader goes away */
    /* Reading end closed means a write gets EPIPE; SIGPIPE is ignored by the
     * caller's own handler, which send(MSG_NOSIGNAL) also covers. */

    mi_outbox *o = mi_outbox_new(1024);
    mi_outbox_push(o, "hello", 5, 0);
    int rc = mi_outbox_flush(o, fds[1]);
    CHECK(rc == -1, "a closed peer is a failure, not backpressure");
    close(fds[1]);
    mi_outbox_free(o);
}

/* A partially written message must be finished before the next one starts. */
static void test_partial_write_resumes(void) {
    int fds[2];
    make_pipe(fds);
    int sz = 4096;
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);

    mi_outbox *o = mi_outbox_new(1 << 20);
    char big[16384];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = (char)('a' + (i % 26));
    CHECK(mi_outbox_push(o, big, sizeof big, 0) == 0, "a message larger than the pipe is queued");

    /* Flush repeatedly, draining in between, until it is all through. */
    size_t total = 0;
    for (int i = 0; i < 100 && !mi_outbox_empty(o); i++) {
        mi_outbox_flush(o, fds[1]);
        size_t len = 0;
        char *got = drain(fds[0], &len);
        /* Every byte received must be the right byte, in the right order: a
         * resume that restarted the message would show up here. */
        for (size_t k = 0; k < len; k++) {
            char want = (char)('a' + ((total + k) % 26));
            if (got[k] != want) {
                CHECK(0, "the resumed bytes are in order");
                break;
            }
        }
        total += len;
        free(got);
    }
    CHECK(total == sizeof big, "the whole message arrived exactly once");
    CHECK(mi_outbox_empty(o), "and the queue drained");
    close(fds[0]);
    close(fds[1]);
    mi_outbox_free(o);
}

int main(void) {
    mi_log_set_level(MI_LOG_ERROR);
    test_basics();
    test_drop_oldest_picture();
    test_state_is_never_dropped();
    test_byte_budget();
    test_full_socket_is_not_an_error();
    test_closed_peer();
    test_partial_write_resumes();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
