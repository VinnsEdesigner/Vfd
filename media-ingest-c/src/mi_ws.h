/* mi_ws.h - the small slice of WebSocket (RFC 6455) Vyzor needs.
 *
 * Vyzor's interactive session has to push a stream of screencast frames to
 * the console and take a stream of input events back, so it needs a full-duplex
 * channel rather than a request/response one. That is what a WebSocket is, and
 * it is the only reason this file exists.
 *
 * WHAT IS DELIBERATELY MISSING
 *
 * No permessage-deflate, no subprotocols, no server-side masking, no
 * continuation reassembly beyond what a caller does itself. A CDP message and a
 * JPEG frame both fit in one frame, so a parser that returns one frame at a
 * time and lets the caller skip what it does not care about is enough. Keeping
 * it small is what makes it testable.
 *
 * The handshake is HTTP: the caller reads the request, checks the upgrade, and
 * asks this module for the Sec-WebSocket-Accept value to echo back.
 */
#ifndef MI_WS_H
#define MI_WS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Opcodes, including the control ones. */
enum {
    MI_WS_CONT = 0x0,   /* continuation of a fragmented message */
    MI_WS_TEXT = 0x1,
    MI_WS_BINARY = 0x2,
    MI_WS_CLOSE = 0x8,
    MI_WS_PING = 0x9,
    MI_WS_PONG = 0xA
};

/*
 * The largest payload this module will assemble into one frame.
 *
 * The protocol allows 2^63 bytes; a peer that claims that must not be believed,
 * because honouring it means allocating whatever it says. Vyzor's own
 * frames are capped well below this, and a control frame may not exceed 125.
 */
#define MI_WS_MAX_PAYLOAD (16u * 1024u * 1024u)

typedef struct {
    int opcode;
    int fin;
    /* Owned, and NUL-terminated one past `len` for text frames so a caller can
     * treat it as a C string without a copy. */
    unsigned char *payload;
    size_t len;
} mi_ws_frame;

/* The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key. Allocated;
 * free it. NULL on allocation failure. */
char *mi_ws_accept_key(const char *client_key);

/*
 * Whether a request's headers describe a WebSocket upgrade.
 *
 * Checks that `Upgrade: websocket` and `Connection: Upgrade` are both present
 * (the Connection header may be a list) and that a Sec-WebSocket-Key is there.
 * Case-insensitive, because header names and tokens are.
 */
int mi_ws_is_upgrade(const char *upgrade, const char *connection, const char *key);

/*
 * Build a frame header (server frames are never masked) into `out`, which must
 * have room for 10 bytes. Returns the header length.
 */
size_t mi_ws_build_header(unsigned char out[10], int opcode, size_t payload_len);

/*
 * Write a whole frame to `fd`.
 *
 * Returns 0 on success, -1 when the peer is gone, and 1 when the socket would
 * block. On 1 the caller must retry the *same* frame — a partially written frame
 * cannot be resumed by a client, so it is never safe to move on.
 */
int mi_ws_send(int fd, int opcode, const void *payload, size_t len);

/*
 * Parse one frame from the front of `buf`.
 *
 * Returns the number of bytes consumed and fills `frame` when a whole frame is
 * present; 0 when more bytes are needed; -1 on a protocol error (bad opcode, an
 * unmasked client frame, a control frame that is too long or fragmented, or a
 * payload larger than MI_WS_MAX_PAYLOAD). A caller loops: read into a buffer,
 * parse, consume, repeat.
 *
 * `frame->payload` is allocated and must be freed with mi_ws_frame_free.
 */
ssize_t mi_ws_parse(const unsigned char *buf, size_t len, mi_ws_frame *frame);

void mi_ws_frame_free(mi_ws_frame *frame);

#endif /* MI_WS_H */
