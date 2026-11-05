/* mi_outbox.c - a bounded outbound queue that drops stale pictures.
 *
 * A ring of messages rather than a byte stream: each message is one serialized
 * frame-pair, and dropping one is only safe if it can be dropped whole. See
 * mi_outbox.h for why dropping is the right policy.
 */
#define _GNU_SOURCE
#include "mi_outbox.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "mi_util.h"

/* One queued message. */
typedef struct {
    unsigned char *data;
    size_t len;
    /* How far into `data` the socket has already accepted. */
    size_t sent;
    int droppable;
} out_msg;

struct mi_outbox {
    out_msg *msgs;
    size_t cap;    /* how many messages the ring can hold */
    size_t head;   /* index of the oldest */
    size_t count;  /* how many are in use */
    size_t bytes;  /* total unsent bytes across all of them */
    size_t max_bytes;
    int fd;
    uint64_t dropped;
    uint64_t would_block;
};

mi_outbox *mi_outbox_new(size_t max_bytes) {
    mi_outbox *o = mi_calloc(1, sizeof *o);
    if (!o) return NULL;
    /* A small fixed ring: the policy keeps at most one picture pending, so a
     * handful of slots is plenty. Sized for a few non-droppable messages plus
     * the picture, with room to spare.
     */
    o->cap = 16;
    o->msgs = mi_calloc(o->cap, sizeof *o->msgs);
    if (!o->msgs) {
        free(o);
        return NULL;
    }
    o->max_bytes = max_bytes ? max_bytes : (256u * 1024u);
    o->fd = -1;
    return o;
}

void mi_outbox_free(mi_outbox *o) {
    if (!o) return;
    for (size_t i = 0; i < o->cap; i++) free(o->msgs[i].data);
    free(o->msgs);
    free(o);
}

/* The index of the i-th message in queue order. */
static size_t idx_of(const mi_outbox *o, size_t i) { return (o->head + i) % o->cap; }

/* Drop the message at queue position `i`, which must be a droppable picture. */
static void drop_at(mi_outbox *o, size_t i) {
    size_t k = idx_of(o, i);
    o->bytes -= (o->msgs[k].len - o->msgs[k].sent);
    free(o->msgs[k].data);
    o->msgs[k].data = NULL;
    o->msgs[k].len = 0;
    o->msgs[k].sent = 0;
    o->msgs[k].droppable = 0;
    /* Close the gap by shifting the later messages down one slot. */
    for (size_t j = i; j + 1 < o->count; j++) {
        o->msgs[idx_of(o, j)] = o->msgs[idx_of(o, j + 1)];
    }
    size_t last = idx_of(o, o->count - 1);
    o->msgs[last].data = NULL;
    o->msgs[last].len = 0;
    o->msgs[last].sent = 0;
    o->msgs[last].droppable = 0;
    o->count--;
    o->dropped++;
}

/* Drop the oldest droppable message. Returns 1 when one was dropped. */
static int drop_oldest_droppable(mi_outbox *o) {
    for (size_t i = 0; i < o->count; i++) {
        if (o->msgs[idx_of(o, i)].droppable) {
            drop_at(o, i);
            return 1;
        }
    }
    return 0;
}

int mi_outbox_push(mi_outbox *o, const void *data, size_t len, int droppable) {
    if (!o || !data || len == 0) return -1;

    /*
     * A new picture supersedes any picture still waiting.
     *
     * This is the rule that keeps a live view live. Without it a slow link
     * accumulates a queue of stale pictures and the viewer falls further and
     * further behind — the delay grows and never recovers, because the page
     * keeps painting at its own rate regardless of how fast the socket drains.
     */
    if (droppable) {
        for (size_t i = 0; i < o->count; i++) {
            if (o->msgs[idx_of(o, i)].droppable) {
                drop_at(o, i);
                break;
            }
        }
    }

    /*
     * Make room, oldest first. Only pictures give way; a state change or a
     * farewell is never dropped, because those carry information rather than a
     * moment.
     */
    while (o->bytes + len > o->max_bytes && drop_oldest_droppable(o)) {
        /* keep going */
    }

    /*
     * If the budget is still exceeded and nothing may be evicted, the message is
     * refused. That keeps the bound real: without it, a client that stopped
     * reading while Vyzor kept reporting state would grow Vyzor's
     * memory without limit. The caller sees the refusal and can drop the session,
     * which is the honest response to a peer that is not reading.
     */
    if (o->bytes + len > o->max_bytes) return -1;

    if (o->count >= o->cap) {
        if (!drop_oldest_droppable(o)) return -1;
    }

    unsigned char *copy = mi_malloc(len);
    if (!copy) return -1;
    memcpy(copy, data, len);
    size_t k = idx_of(o, o->count);
    o->msgs[k].data = copy;
    o->msgs[k].len = len;
    o->msgs[k].sent = 0;
    o->msgs[k].droppable = droppable;
    o->count++;
    o->bytes += len;
    return 0;
}

int mi_outbox_flush(mi_outbox *o, int fd) {
    if (!o) return -1;
    o->fd = fd;
    while (o->count > 0) {
        out_msg *m = &o->msgs[o->head];
        while (m->sent < m->len) {
            ssize_t n = send(fd, m->data + m->sent, m->len - m->sent, MSG_NOSIGNAL);
            if (n > 0) {
                m->sent += (size_t)n;
                o->bytes -= (size_t)n;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* The socket is full. Not an error: the rest waits, and the
                 * caller will be told when the socket is writable again. */
                o->would_block++;
                return 0;
            }
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        free(m->data);
        m->data = NULL;
        m->len = 0;
        m->sent = 0;
        m->droppable = 0;
        o->head = (o->head + 1) % o->cap;
        o->count--;
    }
    return 0;
}

int mi_outbox_empty(const mi_outbox *o) { return !o || o->count == 0; }
size_t mi_outbox_bytes(const mi_outbox *o) { return o ? o->bytes : 0; }
size_t mi_outbox_count(const mi_outbox *o) { return o ? o->count : 0; }
uint64_t mi_outbox_dropped(const mi_outbox *o) { return o ? o->dropped : 0; }
uint64_t mi_outbox_would_block(const mi_outbox *o) { return o ? o->would_block : 0; }
void mi_outbox_set_fd(mi_outbox *o, int fd) {
    if (o) o->fd = fd;
}
int mi_outbox_fd(const mi_outbox *o) { return o ? o->fd : -1; }
