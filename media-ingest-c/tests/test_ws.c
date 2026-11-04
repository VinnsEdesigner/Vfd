/* test_ws.c - WebSocket framing, plus the SHA-1 and base64 it stands on.
 *
 * Everything here is pure: no sockets, no browser, no clock. That is deliberate
 * — a framing bug is a security bug (a length that is trusted, a mask that is
 * not required), and it should be catchable without a network. The vectors are
 * from the RFCs, not from this implementation, so a matching pair of bugs cannot
 * pass by agreeing with itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_util.h"
#include "mi_ws.h"

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

static void test_sha1(void) {
    unsigned char d[20];
    char hex[41];
    /* FIPS 180-1 vectors. */
    mi_sha1("abc", 3, d);
    mi_hex_encode(d, 20, hex);
    CHECK(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1(abc)");

    mi_sha1("", 0, d);
    mi_hex_encode(d, 20, hex);
    CHECK(!strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "sha1(empty)");

    /* Two blocks: 56 bytes forces the two-block padding path. */
    mi_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    mi_hex_encode(d, 20, hex);
    CHECK(!strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "sha1(56-byte vector)");
}

static void test_base64(void) {
    char *e = mi_base64_encode((const unsigned char *)"", 0);
    CHECK(e && !strcmp(e, ""), "base64 of nothing is nothing");
    free(e);

    e = mi_base64_encode((const unsigned char *)"f", 1);
    CHECK(e && !strcmp(e, "Zg=="), "one byte pads with two");
    free(e);

    e = mi_base64_encode((const unsigned char *)"fo", 2);
    CHECK(e && !strcmp(e, "Zm8="), "two bytes pad with one");
    free(e);

    e = mi_base64_encode((const unsigned char *)"foo", 3);
    CHECK(e && !strcmp(e, "Zm9v"), "three bytes need no padding");
    free(e);

    e = mi_base64_encode((const unsigned char *)"hello", 5);
    CHECK(e && !strcmp(e, "aGVsbG8="), "hello encodes");
    free(e);

    /* Round-trip a byte range that includes 0x00 and 0xFF, where a signed-char
     * bug would show. */
    unsigned char raw[256];
    for (int i = 0; i < 256; i++) raw[i] = (unsigned char)i;
    char *enc = mi_base64_encode(raw, sizeof raw);
    unsigned char dec[256];
    ssize_t n = mi_base64_decode(enc, strlen(enc), dec, sizeof dec);
    CHECK(n == 256, "a 256-byte range decodes to the same length");
    CHECK(n == 256 && !memcmp(raw, dec, 256), "and to the same bytes");
    free(enc);

    /* Malformed input is refused, not guessed at. */
    unsigned char out[16];
    CHECK(mi_base64_decode("!!!!", 4, out, sizeof out) < 0, "a non-alphabet byte is refused");
    CHECK(mi_base64_decode("QQ", 2, out, 1) == 1, "a short input still decodes what it can");
}

static void test_accept_key(void) {
    /* The worked example from RFC 6455 section 1.3. */
    char *accept = mi_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(accept && !strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
          "the RFC's handshake example");
    free(accept);

    CHECK(mi_ws_accept_key(NULL) == NULL, "a null key has no accept value");
    CHECK(mi_ws_accept_key("") == NULL, "an empty key has no accept value");
}

static void test_is_upgrade(void) {
    CHECK(mi_ws_is_upgrade("websocket", "Upgrade", "abc"), "the plain upgrade");
    CHECK(mi_ws_is_upgrade("WebSocket", "keep-alive, Upgrade", "abc"),
          "the header names and the token list are case-insensitive");
    CHECK(!mi_ws_is_upgrade("websocket", "keep-alive", "abc"),
          "no Upgrade token in Connection is not an upgrade");
    CHECK(!mi_ws_is_upgrade("h2c", "Upgrade", "abc"), "a different protocol is not ours");
    CHECK(!mi_ws_is_upgrade("websocket", "Upgrade", ""), "a missing key is not an upgrade");
}

/* Build a masked client frame the way a browser would, for the parser to read. */
static size_t build_client_frame(unsigned char *out, int opcode, const unsigned char *payload,
                                 size_t len, const unsigned char mask[4]) {
    size_t n = 0;
    out[n++] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        out[n++] = (unsigned char)(0x80 | len);
    } else {
        out[n++] = (unsigned char)(0x80 | 126);
        out[n++] = (unsigned char)(len >> 8);
        out[n++] = (unsigned char)len;
    }
    memcpy(out + n, mask, 4);
    n += 4;
    for (size_t i = 0; i < len; i++) out[n + i] = payload[i] ^ mask[i & 3];
    return n + len;
}

static void test_parse(void) {
    const unsigned char mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    unsigned char buf[512];
    mi_ws_frame f;

    /* A masked text frame, as a browser sends. */
    const char *hello = "Hello";
    size_t n = build_client_frame(buf, MI_WS_TEXT, (const unsigned char *)hello, 5, mask);
    ssize_t used = mi_ws_parse(buf, n, &f);
    CHECK(used == (ssize_t)n, "a whole frame is consumed");
    CHECK(f.opcode == MI_WS_TEXT, "the opcode is read");
    CHECK(f.fin == 1, "FIN is read");
    CHECK(f.len == 5 && !memcmp(f.payload, hello, 5), "the masked payload is unmasked");
    CHECK(f.payload[f.len] == '\0', "the payload is NUL-terminated for convenience");
    mi_ws_frame_free(&f);

    /* A partial frame is not an error: the caller reads more. */
    CHECK(mi_ws_parse(buf, 1, &f) == 0, "one byte is not enough");
    CHECK(mi_ws_parse(buf, n - 1, &f) == 0, "a truncated payload waits for more");

    /* An unmasked client frame is a protocol error and must be refused: a
     * proxy that forwards it invites cache poisoning. */
    unsigned char plain[16];
    plain[0] = 0x81;
    plain[1] = 0x05; /* no mask bit */
    memcpy(plain + 2, hello, 5);
    CHECK(mi_ws_parse(plain, 7, &f) == -1, "an unmasked client frame is refused");

    /* A control frame longer than 125 bytes is illegal. The payload must be a
     * real 126 bytes: the parser refuses it on the length, but the helper still
     * reads what it is told to, and passing a short literal is a test bug ASan
     * caught. */
    unsigned char big[160];
    unsigned char payload126b[126];
    memset(payload126b, 'x', sizeof payload126b);
    size_t bn = build_client_frame(big, MI_WS_PING, payload126b, 126, mask);
    CHECK(mi_ws_parse(big, bn, &f) == -1, "an over-long control frame is refused");

    /* A 16-bit length must be minimally encoded. */
    unsigned char short16[16];
    short16[0] = 0x81;
    short16[1] = 0x80 | 126;
    short16[2] = 0x00;
    short16[3] = 0x05; /* 5, which fits in 7 bits */
    memcpy(short16 + 4, mask, 4);
    CHECK(mi_ws_parse(short16, 12, &f) == -1, "a non-minimal length is refused");

    /* A 126-byte payload uses the 16-bit form and parses. */
    unsigned char payload126[126];
    memset(payload126, 'a', sizeof payload126);
    size_t n126 = build_client_frame(buf, MI_WS_BINARY, payload126, sizeof payload126, mask);
    used = mi_ws_parse(buf, n126, &f);
    CHECK(used == (ssize_t)n126, "a 126-byte frame parses");
    CHECK(f.len == 126 && !memcmp(f.payload, payload126, 126), "its payload is intact");
    mi_ws_frame_free(&f);
}

static void test_build_header(void) {
    unsigned char h[10];
    CHECK(mi_ws_build_header(h, MI_WS_TEXT, 5) == 2, "a small payload uses two header bytes");
    CHECK(h[0] == 0x81, "FIN and the text opcode");
    CHECK(h[1] == 0x05, "a short length is written directly");

    CHECK(mi_ws_build_header(h, MI_WS_BINARY, 200) == 4, "a medium payload uses four");
    CHECK(h[1] == 126, "the 16-bit marker");
    CHECK(h[2] == 0 && h[3] == 200, "the length is big-endian");

    CHECK(mi_ws_build_header(h, MI_WS_BINARY, 70000) == 10, "a large payload uses ten");
    CHECK(h[1] == 127, "the 64-bit marker");
    CHECK(h[9] == (unsigned char)(70000 & 0xFF), "the low byte of the length");
}

int main(void) {
    mi_log_set_level(MI_LOG_ERROR);
    test_sha1();
    test_base64();
    test_accept_key();
    test_is_upgrade();
    test_parse();
    test_build_header();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
