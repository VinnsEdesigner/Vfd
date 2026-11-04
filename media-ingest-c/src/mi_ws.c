/* mi_ws.c - WebSocket framing (RFC 6455). See mi_ws.h for what is in scope. */
#define _GNU_SOURCE
#include "mi_ws.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "mi_util.h"

/* The fixed GUID from RFC 6455 section 1.3. */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

char *mi_ws_accept_key(const char *client_key) {
    if (!client_key || !*client_key) return NULL;
    size_t klen = strlen(client_key);
    size_t glen = sizeof WS_GUID - 1; /* no NUL */
    char *concat = mi_malloc(klen + glen + 1);
    if (!concat) return NULL;
    memcpy(concat, client_key, klen);
    memcpy(concat + klen, WS_GUID, glen);
    concat[klen + glen] = '\0';

    unsigned char digest[20];
    mi_sha1(concat, klen + glen, digest);
    free(concat);
    return mi_base64_encode(digest, sizeof digest);
}

/* Does a comma-separated token list contain `want` (case-insensitive)? */
static int token_in_list(const char *list, const char *want) {
    if (!list || !want) return 0;
    size_t wlen = strlen(want);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - start) == wlen && !strncasecmp(start, want, wlen)) return 1;
    }
    return 0;
}

int mi_ws_is_upgrade(const char *upgrade, const char *connection, const char *key) {
    if (!upgrade || !connection || !key || !*key) return 0;
    if (strcasecmp(upgrade, "websocket") != 0) return 0;
    return token_in_list(connection, "Upgrade");
}

size_t mi_ws_build_header(unsigned char out[10], int opcode, size_t payload_len) {
    size_t n = 0;
    out[n++] = (unsigned char)(0x80 | (opcode & 0x0F)); /* FIN + opcode */
    if (payload_len < 126) {
        out[n++] = (unsigned char)payload_len;
    } else if (payload_len <= 0xFFFF) {
        out[n++] = 126;
        out[n++] = (unsigned char)(payload_len >> 8);
        out[n++] = (unsigned char)(payload_len);
    } else {
        out[n++] = 127;
        for (int i = 7; i >= 0; i--) out[n++] = (unsigned char)(payload_len >> (8 * i));
    }
    return n;
}

int mi_ws_send(int fd, int opcode, const void *payload, size_t len) {
    unsigned char header[10];
    size_t hlen = mi_ws_build_header(header, opcode, len);
    /* One writev would be nicer, but two sends keep it obvious and the second
     * is a single syscall for the common small frame. */
    size_t sent = 0;
    while (sent < hlen) {
        ssize_t n = send(fd, (const char *)header + sent, hlen - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    const unsigned char *p = payload;
    sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

ssize_t mi_ws_parse(const unsigned char *buf, size_t len, mi_ws_frame *frame) {
    if (len < 2) return 0;

    int fin = (buf[0] & 0x80) != 0;
    int opcode = buf[0] & 0x0F;
    int masked = (buf[1] & 0x80) != 0;
    uint64_t plen = buf[1] & 0x7F;
    size_t pos = 2;

    /* A client frame must be masked; an unmasked one is a protocol error, and
     * accepting it is how a proxy-cache-poisoning bug starts. */
    if (!masked) return -1;

    if (plen == 126) {
        if (len < pos + 2) return 0;
        plen = ((uint64_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;
        /* Not minimally encoded: a value that fits in 7 bits must use them. */
        if (plen < 126) return -1;
    } else if (plen == 127) {
        if (len < pos + 8) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | buf[pos + i];
        pos += 8;
        if (plen <= 0xFFFF) return -1; /* not minimally encoded */
    }

    /* Control frames are short and may not be fragmented. */
    int is_control = opcode >= 0x8;
    if (is_control) {
        if (plen > 125) return -1;
        if (!fin) return -1;
    }
    if (plen > MI_WS_MAX_PAYLOAD) return -1;

    if (len < pos + 4) return 0; /* masking key */
    unsigned char mask[4];
    memcpy(mask, buf + pos, 4);
    pos += 4;

    if (len < pos + plen) return 0; /* payload not all here yet */

    unsigned char *payload = mi_malloc((size_t)plen + 1);
    if (!payload) return -1;
    for (uint64_t i = 0; i < plen; i++) payload[i] = buf[pos + i] ^ mask[i & 3];
    payload[plen] = '\0';

    frame->opcode = opcode;
    frame->fin = fin;
    frame->payload = payload;
    frame->len = (size_t)plen;
    return (ssize_t)(pos + plen);
}

void mi_ws_frame_free(mi_ws_frame *frame) {
    if (!frame) return;
    free(frame->payload);
    frame->payload = NULL;
    frame->len = 0;
}
