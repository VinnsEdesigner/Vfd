/* test_charset.c - sniffing the encoding, and decoding to UTF-8.
 *
 * The fixtures are real bytes in each encoding, so the decode is checked end to
 * end: a Windows-1252 smart quote, a Shift-JIS and a GBK word, and the BOMs.
 * The property that matters is that a page in a non-UTF-8 encoding comes out as
 * correct UTF-8 — which is what the extractor's selectors then run over.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_charset.h"

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

static void test_labels(void) {
    CHECK(mi_charset_from_label("utf-8") == MI_CS_UTF8, "utf-8");
    CHECK(mi_charset_from_label("UTF8") == MI_CS_UTF8, "UTF8");
    CHECK(mi_charset_from_label("ISO-8859-1") == MI_CS_WINDOWS_1252,
          "iso-8859-1 maps to windows-1252, as the spec says");
    CHECK(mi_charset_from_label("latin1") == MI_CS_WINDOWS_1252, "latin1");
    CHECK(mi_charset_from_label("shift_jis") == MI_CS_SHIFT_JIS, "shift_jis");
    CHECK(mi_charset_from_label("gb2312") == MI_CS_GBK, "gb2312 is GBK");
    CHECK(mi_charset_from_label("nonsense") == MI_CS_UNKNOWN, "an unknown label");
}

static void test_bom(void) {
    const unsigned char utf8bom[] = {0xEF, 0xBB, 0xBF, 'h', 'i'};
    size_t bom = 0;
    CHECK(mi_charset_detect(NULL, utf8bom, sizeof utf8bom, MI_CS_UNKNOWN, &bom) == MI_CS_UTF8,
          "a UTF-8 BOM");
    CHECK(bom == 3, "the BOM length is reported");

    const unsigned char le[] = {0xFF, 0xFE, 'h', 0, 'i', 0};
    CHECK(mi_charset_detect(NULL, le, sizeof le, MI_CS_UNKNOWN, &bom) == MI_CS_UTF16LE,
          "a UTF-16LE BOM");
    CHECK(bom == 2, "its length");

    /* The BOM beats a contradictory header. */
    CHECK(mi_charset_detect("text/html; charset=shift_jis", utf8bom, sizeof utf8bom,
                            MI_CS_UNKNOWN, &bom) == MI_CS_UTF8,
          "the BOM overrides the header");
}

static void test_header_and_meta(void) {
    size_t bom = 0;
    CHECK(mi_charset_detect("text/html; charset=windows-1252", (const unsigned char *)"x", 1,
                            MI_CS_UNKNOWN, &bom) == MI_CS_WINDOWS_1252,
          "the header charset");
    {
        const char *m = "<meta charset=\"shift_jis\">";
        CHECK(mi_charset_detect("text/html", (const unsigned char *)m, strlen(m),
                                MI_CS_UNKNOWN, &bom) == MI_CS_SHIFT_JIS,
              "a <meta charset>");
    }
    {
        const char *m = "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=gbk\">";
        CHECK(mi_charset_detect("text/html", (const unsigned char *)m, strlen(m),
                                MI_CS_UNKNOWN, &bom) == MI_CS_GBK,
              "a <meta http-equiv> content-type");
    }
    /* The header beats the meta. */
    {
        const char *m = "<meta charset=\"shift_jis\">";
        CHECK(mi_charset_detect("text/html; charset=windows-1252", (const unsigned char *)m,
                                strlen(m), MI_CS_UNKNOWN, &bom) == MI_CS_WINDOWS_1252,
              "the header wins over a meta");
    }
    /* Nothing said: the caller's default, then UTF-8. */
    CHECK(mi_charset_detect("text/html", (const unsigned char *)"<p>x</p>", 8,
                            MI_CS_ISO_8859_2, &bom) == MI_CS_ISO_8859_2,
          "the caller's default is used");
    CHECK(mi_charset_detect("text/html", (const unsigned char *)"<p>x</p>", 8,
                            MI_CS_UNKNOWN, &bom) == MI_CS_UTF8,
          "and UTF-8 when there is nothing else");
}

static void test_decode_1252(void) {
    /* 0x93/0x94 are the smart quotes in windows-1252 — invalid as UTF-8. */
    const unsigned char in[] = {'a', 0x93, 'q', 0x94, ' ', 0xE9};
    char *out = mi_charset_to_utf8(MI_CS_WINDOWS_1252, in, sizeof in);
    CHECK(out != NULL, "decodes");
    CHECK(strstr(out, "\xe2\x80\x9c") != NULL, "the left double quote is U+201C");
    CHECK(strstr(out, "\xc3\xa9") != NULL, "0xE9 is é as UTF-8");
    free(out);
}

static void test_decode_utf8_roundtrip(void) {
    const char *src = "héllo wörld — 日本";
    char *out = mi_charset_to_utf8(MI_CS_UTF8, (const unsigned char *)src, strlen(src));
    CHECK(out && !strcmp(out, src), "UTF-8 passes through unchanged");
    free(out);
}

static void test_decode_multibyte(void) {
    /* Shift-JIS for "日本" is 0x93 0xFA 0x96 0x7B. */
    const unsigned char sjis[] = {'x', 0x93, 0xFA, 0x96, 0x7B};
    char *out = mi_charset_to_utf8(MI_CS_SHIFT_JIS, sjis, sizeof sjis);
    CHECK(out != NULL, "shift-jis decodes");
    CHECK(strstr(out, "\xe6\x97\xa5\xe6\x9c\xac") != NULL,
          "the shift-jis bytes are 日本 in UTF-8");
    free(out);

    /* GBK for "中文" is 0xD6 0xD0 0xCE 0xC4. */
    const unsigned char gbk[] = {0xD6, 0xD0, 0xCE, 0xC4};
    out = mi_charset_to_utf8(MI_CS_GBK, gbk, sizeof gbk);
    CHECK(out && strstr(out, "\xe4\xb8\xad\xe6\x96\x87") != NULL,
          "the GBK bytes are 中文 in UTF-8");
    free(out);
}

static void test_decode_document(void) {
    const unsigned char in[] = {'h', 'i', 0x93};
    mi_charset cs = MI_CS_UNKNOWN;
    char *out = mi_charset_decode_document("text/html; charset=windows-1252", in, sizeof in,
                                           MI_CS_UNKNOWN, &cs);
    CHECK(cs == MI_CS_WINDOWS_1252, "the document path sniffs the encoding");
    CHECK(out && strstr(out, "\xe2\x80\x9c") != NULL, "and decodes it");
    free(out);
}

/* A bad byte in the middle does not lose the rest of the page. */
static void test_bad_byte_recovery(void) {
    const unsigned char in[] = {'a', 0xFF, 'b', 0xFE, 'c'};
    char *out = mi_charset_to_utf8(MI_CS_UTF8, in, sizeof in);
    CHECK(out != NULL, "a bad UTF-8 byte does not fail the decode");
    /* The ASCII on both sides survives. */
    CHECK(out[0] == 'a', "the byte before is kept");
    CHECK(strchr(out, 'c') != NULL, "the byte after is kept");
    free(out);
}

int main(void) {
    test_labels();
    test_bom();
    test_header_and_meta();
    test_decode_1252();
    test_decode_utf8_roundtrip();
    test_decode_multibyte();
    test_decode_document();
    test_bad_byte_recovery();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
