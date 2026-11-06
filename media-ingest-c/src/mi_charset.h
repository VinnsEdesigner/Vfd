/* mi_charset.h - decide a page's encoding, and decode it to UTF-8.
 *
 * The fetch layer returns raw bytes and the parser assumes UTF-8. That is wrong
 * for a large slice of the web: a Windows-1252 or Shift-JIS or GBK page comes
 * out as mojibake, and — worse — the *extracted data* is wrong, because a
 * selector runs over the mis-decoded bytes.
 *
 * This is the HTML Standard's encoding-sniffing algorithm, in the order it
 * prescribes:
 *
 *   1. a BOM, which is authoritative and overrides everything;
 *   2. the transport layer — a `charset` in the Content-Type header;
 *   3. a prescan of the first bytes for `<meta charset>` or
 *      `<meta http-equiv="content-type" content="...charset=...">`;
 *   4. otherwise the caller's default, then UTF-8.
 *
 * The labels are normalised (the spec's "get an encoding" step), and a label we
 * cannot decode is not guessed at: an unknown label falls through to the default
 * rather than producing a half-decoded page.
 *
 * Only the encodings that appear on real pages are supported: UTF-8/16, the
 * Windows single-byte set, ISO-8859-1/2/5/7/9/15, KOI8-R/U, and the CJK
 * multi-byte sets Shift-JIS, EUC-JP, EUC-KR and GBK/GB18030. A page in something
 * else is decoded as the default, which is the honest answer for a decoder that
 * does not know the mapping.
 */
#ifndef MI_CHARSET_H
#define MI_CHARSET_H

#include <stddef.h>

/* An encoding the decoder understands, or MI_CS_UNKNOWN. */
typedef enum {
    MI_CS_UNKNOWN = 0,
    MI_CS_UTF8,
    MI_CS_UTF16LE,
    MI_CS_UTF16BE,
    MI_CS_WINDOWS_1252,
    MI_CS_ISO_8859_1,
    MI_CS_ISO_8859_2,
    MI_CS_ISO_8859_5,
    MI_CS_ISO_8859_7,
    MI_CS_ISO_8859_9,
    MI_CS_ISO_8859_15,
    MI_CS_KOI8_R,
    MI_CS_KOI8_U,
    MI_CS_SHIFT_JIS,
    MI_CS_EUC_JP,
    MI_CS_EUC_KR,
    MI_CS_GBK,
} mi_charset;

/* The canonical name of an encoding, for logs and JSON. Never NULL. */
const char *mi_charset_name(mi_charset cs);

/* An encoding from a label such as "utf-8", "ISO-8859-1", "gb2312". */
mi_charset mi_charset_from_label(const char *label);

/*
 * Decide the encoding of a document from everything known about it.
 *
 * `content_type` is the response's Content-Type header (may be NULL);
 * `bytes`/`len` the body's first bytes (may be NULL); `default_cs` the caller's
 * preference when nothing else says (MI_CS_UNKNOWN means UTF-8).
 *
 * Returns the chosen encoding, and sets `*bom_len` to the number of leading BOM
 * bytes to skip (0 when none).
 */
mi_charset mi_charset_detect(const char *content_type, const unsigned char *bytes,
                             size_t len, mi_charset default_cs, size_t *bom_len);

/*
 * Decode `in` (length `len`) to a new NUL-terminated UTF-8 string.
 *
 * Bytes that cannot be decoded become U+FFFD, as a browser does — a single bad
 * byte does not lose the rest of the page. A leading BOM is skipped. The caller
 * frees the result.
 */
char *mi_charset_to_utf8(mi_charset cs, const unsigned char *in, size_t len);

/*
 * The whole path in one call: detect the encoding from the header and the bytes,
 * then decode to UTF-8. `out_encoding` (optional) receives what was chosen, for
 * reporting. Returns the decoded string, or NULL on allocation failure.
 */
char *mi_charset_decode_document(const char *content_type,
                                 const unsigned char *bytes, size_t len,
                                 mi_charset default_cs, mi_charset *out_encoding);

#endif
