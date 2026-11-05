/* mi_outbox.h - a bounded outbound queue for a live session.
 *
 * WHY THIS EXISTS
 *
 * A live session pushes pictures to a browser over a socket. The socket can
 * always be slower than the browser that paints into it, and when it is, the
 * Vyzor has three options and only one of them is right:
 *
 *   block         stop reading from Chromium while the socket drains. Simple,
 *                 and wrong: the whole session stalls, including the input the
 *                 user is waiting on.
 *   queue         keep every frame until it can be sent. Also wrong: a slow link
 *                 builds a backlog, and the user then watches the past. The
 *                 delay grows without bound and never recovers.
 *   drop          discard pictures that have been overtaken and send the newest
 *                 one. Right for a live view: the user wants to see *now*, not
 *                 every frame in between. A dropped frame costs nothing because
 *                 the next one carries the same kind of information.
 *
 * This is the third. `droppable` messages — pictures — are discarded oldest-first
 * when the queue is full or when a newer picture arrives. Everything else (a
 * page state, a farewell) is never dropped, because losing one of those loses
 * information rather than a moment.
 *
 * A message is enqueued already serialized, as one contiguous run of bytes. That
 * is deliberate: a picture is a text header frame followed by a binary frame,
 * and dropping one without the other would leave the client expecting bytes that
 * never come. Serializing first makes a message indivisible, so dropping is
 * always safe.
 */
#ifndef MI_OUTBOX_H
#define MI_OUTBOX_H

#include <stddef.h>
#include <stdint.h>

typedef struct mi_outbox mi_outbox;

/*
 * A queue holding at most `max_bytes` of pending messages.
 *
 * The bound is what makes this safe: without it, a client that stops reading
 * while the page keeps painting would grow Vyzor's memory until it died.
 * A few hundred kilobytes holds several pictures, which is more than a live view
 * needs and far less than a leak.
 */
mi_outbox *mi_outbox_new(size_t max_bytes);
void mi_outbox_free(mi_outbox *o);

/*
 * Append a message. Takes a copy, so the caller may free `data` at once.
 *
 * `droppable` marks a message that may be discarded in favour of a newer one of
 * the same kind — a picture. Returns 0, or -1 when out of memory.
 *
 * A droppable message is dropped when the queue is over its bound, and an older
 * droppable message is dropped when a new one arrives: at most one picture is
 * ever pending behind the one being written, so the delay a viewer sees is one
 * frame rather than a backlog.
 */
int mi_outbox_push(mi_outbox *o, const void *data, size_t len, int droppable);

/*
 * Write as much as the socket will take.
 *
 * Returns 0 whether or not everything went (a full socket is not an error), and
 * -1 when the peer is gone. Call it again after the socket reports writable.
 */
int mi_outbox_flush(mi_outbox *o, int fd);

/* Whether anything is still waiting. */
int mi_outbox_empty(const mi_outbox *o);

/* Bytes waiting, for reporting. */
size_t mi_outbox_bytes(const mi_outbox *o);

/* How many messages are waiting, for reporting. */
size_t mi_outbox_count(const mi_outbox *o);

/*
 * Counters, for reporting rather than guessing.
 *
 * `dropped` is pictures discarded to keep the view live; `would_block` is how
 * often the socket was full. A rising `dropped` on a healthy link means the
 * frame rate is too high for the link, which is a tuning problem, not a fault.
 */
uint64_t mi_outbox_dropped(const mi_outbox *o);
uint64_t mi_outbox_would_block(const mi_outbox *o);

/* The descriptor the queue should be polled for writability on. */
void mi_outbox_set_fd(mi_outbox *o, int fd);
int mi_outbox_fd(const mi_outbox *o);

#endif /* MI_OUTBOX_H */
