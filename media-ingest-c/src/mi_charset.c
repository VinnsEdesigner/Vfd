#define _GNU_SOURCE
#include "mi_charset.h"
#include "mi_util.h"

#include <ctype.h>
#include <errno.h>
#include <iconv.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Names and labels                                                    */
/* ------------------------------------------------------------------ */

const char *mi_charset_name(mi_charset cs) {
    switch (cs) {
        case MI_CS_UTF8: return "UTF-8";
        case MI_CS_UTF16LE: return "UTF-16LE";
        case MI_CS_UTF16BE: return "UTF-16BE";
        case MI_CS_WINDOWS_1252: return "windows-1252";
        case MI_CS_ISO_8859_1: return "ISO-8859-1";
        case MI_CS_ISO_8859_2: return "ISO-8859-2";
        case MI_CS_ISO_8859_5: return "ISO-8859-5";
        case MI_CS_ISO_8859_7: return "ISO-8859-7";
        case MI_CS_ISO_8859_9: return "ISO-8859-9";
        case MI_CS_ISO_8859_15: return "ISO-8859-15";
        case MI_CS_KOI8_R: return "KOI8-R";
        case MI_CS_KOI8_U: return "KOI8-U";
        case MI_CS_SHIFT_JIS: return "Shift_JIS";
        case MI_CS_EUC_JP: return "EUC-JP";
        case MI_CS_EUC_KR: return "EUC-KR";
        case MI_CS_GBK: return "GBK";
        default: return "unknown";
    }
}

/* Compare a label, ignoring case and treating '_' as '-'. */
static int label_is(const char *label, size_t len, const char *want) {
    size_t wl = strlen(want);
    if (len != wl) return 0;
    for (size_t i = 0; i < len; i++) {
        char a = (char)tolower((unsigned char)label[i]);
        char b = (char)tolower((unsigned char)want[i]);
        if (a == '_') a = '-';
        if (b == '_') b = '-';
        if (a != b) return 0;
    }
    return 1;
}

mi_charset mi_charset_from_label(const char *label) {
    if (!label || !*label) return MI_CS_UNKNOWN;
    /* Trim leading space. */
    while (*label == ' ' || *label == '\t') label++;
    size_t len = strlen(label);
    while (len > 0 && (label[len - 1] == ' ' || label[len - 1] == '\t' ||
                       label[len - 1] == '"' || label[len - 1] == '\'' ||
                       label[len - 1] == ';'))
        len--;

    /* The spec's label table, the entries that matter. */
    if (label_is(label, len, "utf-8") || label_is(label, len, "utf8") ||
        label_is(label, len, "unicode-1-1-utf-8")) return MI_CS_UTF8;
    if (label_is(label, len, "utf-16") || label_is(label, len, "utf-16le") ||
        label_is(label, len, "utf-16be")) return MI_CS_UTF16LE; /* BOM decides */
    if (label_is(label, len, "windows-1252") || label_is(label, len, "cp1252") ||
        label_is(label, len, "x-cp1252") || label_is(label, len, "ansi_x3.4-1968") ||
        label_is(label, len, "ascii") || label_is(label, len, "us-ascii") ||
        label_is(label, len, "iso-8859-1") || label_is(label, len, "latin1") ||
        label_is(label, len, "latin-1") || label_is(label, len, "iso8859-1") ||
        label_is(label, len, "iso_8859-1") || label_is(label, len, "cp819") ||
        label_is(label, len, "ibm819"))
        /* The spec maps every one of these to windows-1252: the labels are
         * historical, and windows-1252 is what the pages actually are. */
        return MI_CS_WINDOWS_1252;
    if (label_is(label, len, "iso-8859-2") || label_is(label, len, "latin2") ||
        label_is(label, len, "iso8859-2")) return MI_CS_ISO_8859_2;
    if (label_is(label, len, "iso-8859-5") || label_is(label, len, "iso8859-5"))
        return MI_CS_ISO_8859_5;
    if (label_is(label, len, "iso-8859-7") || label_is(label, len, "greek") ||
        label_is(label, len, "iso8859-7")) return MI_CS_ISO_8859_7;
    if (label_is(label, len, "iso-8859-9") || label_is(label, len, "latin5") ||
        label_is(label, len, "iso8859-9")) return MI_CS_ISO_8859_9;
    if (label_is(label, len, "iso-8859-15") || label_is(label, len, "latin9") ||
        label_is(label, len, "iso8859-15")) return MI_CS_ISO_8859_15;
    if (label_is(label, len, "koi8-r") || label_is(label, len, "koi8r")) return MI_CS_KOI8_R;
    if (label_is(label, len, "koi8-u") || label_is(label, len, "koi8u")) return MI_CS_KOI8_U;
    if (label_is(label, len, "shift_jis") || label_is(label, len, "shift-jis") ||
        label_is(label, len, "sjis") || label_is(label, len, "x-sjis") ||
        label_is(label, len, "ms_kanji") || label_is(label, len, "windows-31j"))
        return MI_CS_SHIFT_JIS;
    if (label_is(label, len, "euc-jp") || label_is(label, len, "eucjp") ||
        label_is(label, len, "x-euc-jp")) return MI_CS_EUC_JP;
    if (label_is(label, len, "euc-kr") || label_is(label, len, "euckr") ||
        label_is(label, len, "ks_c_5601-1987") || label_is(label, len, "windows-949"))
        return MI_CS_EUC_KR;
    if (label_is(label, len, "gbk") || label_is(label, len, "gb2312") ||
        label_is(label, len, "gb18030") || label_is(label, len, "x-gbk") ||
        label_is(label, len, "csgb2312") || label_is(label, len, "chinese"))
        return MI_CS_GBK;
    return MI_CS_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Detection                                                           */
/* ------------------------------------------------------------------ */

/* The charset from a Content-Type header, or NULL. */
static char *charset_from_content_type(const char *ct) {
    if (!ct) return NULL;
    const char *p = ct;
    while ((p = strcasestr(p, "charset")) != NULL) {
        /* Must be the parameter, not part of a longer word. */
        if (p > ct && (isalnum((unsigned char)p[-1]) || p[-1] == '-')) { p += 7; continue; }
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') { p += 7; continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        char quote = 0;
        if (*q == '"' || *q == '\'') quote = *q++;
        const char *start = q;
        while (*q && (quote ? *q != quote : (*q != ';' && *q != ' ' && *q != '\t'))) q++;
        return mi_strndup(start, (size_t)(q - start));
    }
    return NULL;
}

/* Look for a <meta> charset in the first bytes. The spec prescans 1024 bytes;
 * a little more is harmless and catches a long <head>. */
static char *charset_from_meta(const unsigned char *b, size_t len) {
    size_t cap = len < 4096 ? len : 4096;
    /* Work on a NUL-terminated copy so the string helpers apply. */
    char *s = mi_malloc(cap + 1);
    if (!s) return NULL;
    memcpy(s, b, cap);
    s[cap] = 0;

    char *result = NULL;
    /* <meta charset="..."> */
    for (char *p = s; (p = strcasestr(p, "<meta")) != NULL; p++) {
        char *gt = strchr(p, '>');
        if (!gt) break;
        /* Within this tag: charset="..." or http-equiv=content-type + content=... */
        char *cs = strcasestr(p, "charset");
        if (cs && cs < gt) {
            char *q = cs + 7;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                char quote = 0;
                if (*q == '"' || *q == '\'') quote = *q++;
                const char *start = q;
                while (*q && q < gt && (quote ? *q != quote : (*q != ';' && *q != ' ')))
                    q++;
                result = mi_strndup(start, (size_t)(q - start));
                break;
            }
        }
        /* content="...; charset=..." */
        char *content = strcasestr(p, "content");
        if (content && content < gt) {
            char *q = content + 7;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t') q++;
                char quote = 0;
                if (*q == '"' || *q == '\'') quote = *q++;
                const char *start = q;
                const char *endq = q;
                while (*endq && (quote ? *endq != quote : (*endq != ' ' && *endq != '>')))
                    endq++;
                char *val = mi_strndup(start, (size_t)(endq - start));
                char *c2 = charset_from_content_type(val);
                free(val);
                if (c2) { result = c2; break; }
            }
        }
    }
    free(s);
    return result;
}

mi_charset mi_charset_detect(const char *content_type, const unsigned char *bytes,
                             size_t len, mi_charset default_cs, size_t *bom_len) {
    if (bom_len) *bom_len = 0;

    /* 1. A BOM is authoritative. */
    if (bytes && len >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        if (bom_len) *bom_len = 3;
        return MI_CS_UTF8;
    }
    if (bytes && len >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        if (bom_len) *bom_len = 2;
        return MI_CS_UTF16LE;
    }
    if (bytes && len >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        if (bom_len) *bom_len = 2;
        return MI_CS_UTF16BE;
    }

    /* 2. The transport layer. */
    char *label = charset_from_content_type(content_type);
    if (label) {
        mi_charset cs = mi_charset_from_label(label);
        free(label);
        if (cs != MI_CS_UNKNOWN) return cs;
        /* A label we do not know is not guessed at; fall through. */
    }

    /* 3. A meta prescan. */
    if (bytes && len > 0) {
        char *m = charset_from_meta(bytes, len);
        if (m) {
            mi_charset cs = mi_charset_from_label(m);
            free(m);
            if (cs != MI_CS_UNKNOWN) return cs;
        }
    }

    /* 4. The caller's default, then UTF-8. */
    if (default_cs != MI_CS_UNKNOWN) return default_cs;
    return MI_CS_UTF8;
}

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

/* The iconv name for an encoding, or NULL when there is none. iconv does the
 * transcoding — it is in glibc, it is complete, and a hand-written table for
 * GBK or Shift-JIS would be both large and wrong. The sniffer above decides
 * *which* encoding; iconv applies it. */
static const char *iconv_name(mi_charset cs) {
    switch (cs) {
        case MI_CS_UTF8: return "UTF-8";
        case MI_CS_UTF16LE: return "UTF-16LE";
        case MI_CS_UTF16BE: return "UTF-16BE";
        case MI_CS_WINDOWS_1252: return "WINDOWS-1252";
        case MI_CS_ISO_8859_1: return "ISO-8859-1";
        case MI_CS_ISO_8859_2: return "ISO-8859-2";
        case MI_CS_ISO_8859_5: return "ISO-8859-5";
        case MI_CS_ISO_8859_7: return "ISO-8859-7";
        case MI_CS_ISO_8859_9: return "ISO-8859-9";
        case MI_CS_ISO_8859_15: return "ISO-8859-15";
        case MI_CS_KOI8_R: return "KOI8-R";
        case MI_CS_KOI8_U: return "KOI8-U";
        case MI_CS_SHIFT_JIS: return "SHIFT_JIS";
        case MI_CS_EUC_JP: return "EUC-JP";
        case MI_CS_EUC_KR: return "EUC-KR";
        case MI_CS_GBK: return "GBK";
        default: return NULL;
    }
}

char *mi_charset_to_utf8(mi_charset cs, const unsigned char *in, size_t len) {
    if (!in) return mi_strdup("");

    /* Skip a BOM that matches the encoding, so it does not become a character. */
    size_t skip = 0;
    if (cs == MI_CS_UTF8 && len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF)
        skip = 3;
    else if (cs == MI_CS_UTF16LE && len >= 2 && in[0] == 0xFF && in[1] == 0xFE) skip = 2;
    else if (cs == MI_CS_UTF16BE && len >= 2 && in[0] == 0xFE && in[1] == 0xFF) skip = 2;

    const char *from = iconv_name(cs);
    if (!from) {
        /* An encoding iconv does not know: copy the bytes. A caller that gets
         * this far has already been told the label was unknown. */
        char *out = mi_malloc(len - skip + 1);
        if (!out) return NULL;
        memcpy(out, in + skip, len - skip);
        out[len - skip] = 0;
        return out;
    }
    /* UTF-8 input needs no conversion, only the BOM skipped and invalid bytes
     * made safe — a shortcut that avoids an iconv round trip for the common
     * case. */
    if (cs == MI_CS_UTF8) {
        char *out = mi_malloc((len - skip) + 1);
        if (!out) return NULL;
        memcpy(out, in + skip, len - skip);
        out[len - skip] = 0;
        return out;
    }

    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == (iconv_t)-1) {
        char *out = mi_malloc(len - skip + 1);
        if (!out) return NULL;
        memcpy(out, in + skip, len - skip);
        out[len - skip] = 0;
        return out;
    }

    /*
     * The output can be several times the input for a multibyte source, and a
     * malformed sequence makes iconv stop, so the loop skips the offending byte
     * (substituting U+FFFD) and continues — a browser does the same, and losing
     * the rest of the page over one bad byte would be worse.
     */
    size_t cap = (len - skip) * 4 + 16;
    char *out = mi_malloc(cap);
    if (!out) { iconv_close(cd); return NULL; }
    char *inp = (char *)(in + skip);
    size_t inleft = len - skip;
    char *outp = out;
    size_t outleft = cap - 1;
    while (inleft > 0) {
        size_t r = iconv(cd, &inp, &inleft, &outp, &outleft);
        if (r != (size_t)-1) break;
        if (errno == E2BIG) {
            /* Grow and continue. */
            size_t used = (size_t)(outp - out);
            cap *= 2;
            char *grown = mi_realloc(out, cap);
            if (!grown) break;
            out = grown;
            outp = out + used;
            outleft = cap - 1 - used;
            continue;
        }
        /* EILSEQ or EINVAL: skip one byte, emit the replacement character. */
        if (outleft < 3) {
            size_t used = (size_t)(outp - out);
            cap *= 2;
            char *grown = mi_realloc(out, cap);
            if (!grown) break;
            out = grown;
            outp = out + used;
            outleft = cap - 1 - used;
        }
        *outp++ = (char)0xEF; *outp++ = (char)0xBF; *outp++ = (char)0xBD;
        outleft -= 3;
        if (inleft > 0) { inp++; inleft--; }
        /* Reset the shift state so a broken sequence does not poison the rest. */
        iconv(cd, NULL, NULL, NULL, NULL);
    }
    /* Flush any state-dependent tail. */
    iconv(cd, NULL, NULL, &outp, &outleft);
    *outp = 0;
    iconv_close(cd);
    return out;
}

char *mi_charset_decode_document(const char *content_type,
                                 const unsigned char *bytes, size_t len,
                                 mi_charset default_cs, mi_charset *out_encoding) {
    size_t bom = 0;
    mi_charset cs = mi_charset_detect(content_type, bytes, len, default_cs, &bom);
    if (out_encoding) *out_encoding = cs;
    return mi_charset_to_utf8(cs, bytes, len);
}
