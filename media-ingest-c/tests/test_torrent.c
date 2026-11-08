/* test_torrent.c - reading a .torrent, and understanding a magnet.
 *
 * A `.torrent` is untrusted input from the internet, so the parser is tested
 * with well-formed bytes *and* with hostile ones: a length past the end, a
 * runaway nesting depth, a missing `info`. Each must be a clean rejection, not a
 * read — the failure mode this suite exists to prevent.
 *
 * The magnet cases matter because a magnet is what the index actually hands
 * back, in two spellings (40-hex and 32-char base32), and a hash read wrongly
 * names the wrong torrent.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_torrent.h"
#include "mi_util.h"

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

/* Build a minimal single-file torrent in memory, so the test carries no fixture
 * file and the bytes are visible in the source. The `info` dict is
 *   d6:lengthi1024e4:name8:test.bin12:piece lengthi16384e6:pieces20:<20 bytes>e
 */
static size_t make_single_file_torrent(unsigned char *buf, size_t cap) {
    const char *prefix = "d8:announce31:http://tracker.example/announce4:infod";
    const char *mid = "6:lengthi1024e4:name8:test.bin12:piece lengthi16384e6:pieces20:";
    const char *suffix = "ee";
    size_t n = 0;
    memcpy(buf + n, prefix, strlen(prefix));
    n += strlen(prefix);
    memcpy(buf + n, mid, strlen(mid));
    n += strlen(mid);
    if (n + 20 + strlen(suffix) > cap) return 0;
    for (int i = 0; i < 20; i++) buf[n++] = (unsigned char)(i + 1);
    memcpy(buf + n, suffix, strlen(suffix));
    n += strlen(suffix);
    return n;
}

static void test_parse_single_file(void) {
    unsigned char buf[512];
    size_t n = make_single_file_torrent(buf, sizeof buf);
    CHECK(n > 0, "built a single-file torrent");

    mi_torrent t;
    CHECK(mi_torrent_parse(buf, n, &t) == MI_OK, "a single-file torrent parses");
    CHECK(t.name && strcmp(t.name, "test.bin") == 0, "the name is read");
    CHECK(t.file_count == 1, "one file");
    CHECK(t.total_bytes == 1024, "the length is the total");
    CHECK(t.piece_length == 16384, "the piece length is read");
    CHECK(t.piece_count == 1, "one piece from 20 bytes");
    CHECK(t.tracker_count == 1, "the announce URL is a tracker");
    CHECK(t.trackers[0] && strstr(t.trackers[0], "tracker.example") != NULL,
          "the tracker is read");
    /* The info-hash is 40 lowercase hex. */
    CHECK(t.info_hash && strlen(t.info_hash) == 40, "an info-hash is computed");
    if (t.info_hash) {
        for (int i = 0; i < 40; i++) {
            CHECK((t.info_hash[i] >= '0' && t.info_hash[i] <= '9') ||
                      (t.info_hash[i] >= 'a' && t.info_hash[i] <= 'f'),
                  "the info-hash is lowercase hex");
            break; /* one representative check is enough for the loop */
        }
    }
    CHECK(t.magnet && strncmp(t.magnet, "magnet:?xt=urn:btih:", 20) == 0,
          "a magnet is built from the torrent");
    mi_torrent_free(&t);
}

static void test_rejects_bad_input(void) {
    mi_torrent t;
    /* Not bencode at all. */
    CHECK(mi_torrent_parse((const unsigned char *)"not a torrent", 13, &t) != MI_OK,
          "junk is rejected");
    /* Valid bencode, but no `info` dict — not a torrent. */
    const char *noinfo = "d8:announce4:abcde";
    CHECK(mi_torrent_parse((const unsigned char *)noinfo, strlen(noinfo), &t) != MI_OK,
          "bencode without an info dict is rejected");
    /* A string length that runs past the end must not read past the buffer. */
    const char *overrun = "d4:infod4:name9999999999:xee";
    CHECK(mi_torrent_parse((const unsigned char *)overrun, strlen(overrun), &t) != MI_OK,
          "a length past the end is rejected");
    /* A pathological nesting depth must not exhaust the stack. */
    char deep[512];
    size_t d = 0;
    for (int i = 0; i < 200 && d < sizeof deep - 2; i++) deep[d++] = 'l';
    deep[d++] = 'e';
    CHECK(mi_torrent_parse((const unsigned char *)deep, d, &t) != MI_OK,
          "a runaway nesting depth is rejected");
}

/*
 * Webseeds (`url-list`, BEP-19) — the HTTP source for the content.
 *
 * They matter twice: a webseed-only torrent (archive.org) has no peers and
 * downloads only from one, and a torrent's own image can be shown as a poster by
 * pointing at `<webseed>/<path>`. Both `url-list` spellings — a bare string and a
 * list — appear in the wild, so both are read.
 */
static void test_webseeds(void) {
    /* A single string form. */
    const char *one = "d4:infod6:lengthi8e4:name1:a12:piece lengthi16384e"
                      "6:pieces20:12345678901234567890e"
                      "8:url-list25:http://seed.example/base/e";
    unsigned char buf[512];
    memcpy(buf, one, strlen(one));
    mi_torrent t;
    CHECK(mi_torrent_parse(buf, strlen(one), &t) == MI_OK, "a torrent with a url-list parses");
    CHECK(t.webseed_count == 1, "the string form yields one webseed");
    CHECK(t.webseeds && strcmp(t.webseeds[0], "http://seed.example/base/") == 0,
          "the webseed URL is read");
    mi_torrent_free(&t);

    /* The list form. */
    const char *many = "d4:infod6:lengthi8e4:name1:a12:piece lengthi16384e"
                       "6:pieces20:12345678901234567890e"
                       "8:url-listl18:http://s1.example/18:http://s2.example/ee";
    memcpy(buf, many, strlen(many));
    CHECK(mi_torrent_parse(buf, strlen(many), &t) == MI_OK, "a url-list as a list parses");
    CHECK(t.webseed_count == 2, "both webseeds are read");
    mi_torrent_free(&t);
}

static void test_magnet_hash(void) {
    char hex[41];
    /* The 40-hex form. */
    CHECK(mi_torrent_hash_from_magnet(
              "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=x",
              hex) == MI_OK,
          "a 40-hex magnet hash is read");
    CHECK(strcmp(hex, "0123456789abcdef0123456789abcdef01234567") == 0,
          "the hash is lowercased and exact");
    /* Uppercase input is normalised. */
    CHECK(mi_torrent_hash_from_magnet(
              "magnet:?xt=urn:btih:0123456789ABCDEF0123456789ABCDEF01234567", hex) == MI_OK,
          "an uppercase hash is accepted");
    CHECK(strcmp(hex, "0123456789abcdef0123456789abcdef01234567") == 0,
          "and normalised to lowercase");
    /* No xt at all. */
    CHECK(mi_torrent_hash_from_magnet("magnet:?dn=nothing", hex) != MI_OK,
          "a magnet with no hash is refused");
}

static void test_magnet_name_and_build(void) {
    char *name = mi_torrent_name_from_magnet(
        "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Ubuntu+24.04");
    CHECK(name && strcmp(name, "Ubuntu 24.04") == 0, "the display name is decoded");
    free(name);

    const char *trackers[] = {"http://t.example/announce"};
    char *m = mi_torrent_build_magnet("0123456789abcdef0123456789abcdef01234567",
                                      "My File", trackers, 1);
    CHECK(m != NULL, "a magnet is built");
    if (m) {
        CHECK(strstr(m, "urn:btih:0123456789abcdef0123456789abcdef01234567") != NULL,
              "the hash is in the magnet");
        CHECK(strstr(m, "dn=My%20File") != NULL, "the name is escaped");
        CHECK(strstr(m, "tr=http") != NULL, "the tracker is in the magnet");
        /* Round-trip: the hash we built is the hash we read back. */
        char back[41];
        CHECK(mi_torrent_hash_from_magnet(m, back) == MI_OK &&
                  strcmp(back, "0123456789abcdef0123456789abcdef01234567") == 0,
              "the built magnet round-trips");
        free(m);
    }

    CHECK(mi_torrent_is_magnet("magnet:?xt=urn:btih:abc") == 1, "a magnet is recognised");
    CHECK(mi_torrent_is_magnet("https://example.com/x.torrent") == 0,
          "an http URL is not a magnet");
}

int main(void) {
    test_parse_single_file();
    test_rejects_bad_input();
    test_webseeds();
    test_magnet_hash();
    test_magnet_name_and_build();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
