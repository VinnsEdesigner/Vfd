#define _GNU_SOURCE
#include "mi_lang.h"
#include "mi_json.h"
#include "mi_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* The primary subtag of a language tag, lowercased: "en-US" -> "en". */
static char *primary_subtag(const char *tag) {
    if (!tag || !*tag) return NULL;
    while (*tag == ' ' || *tag == '\t') tag++;
    const char *end = tag;
    while (*end && *end != '-' && *end != '_' && *end != ',' && *end != ';' &&
           !isspace((unsigned char)*end))
        end++;
    if (end == tag) return NULL;
    char *s = mi_strndup(tag, (size_t)(end - tag));
    for (char *q = s; *q; q++) *q = (char)tolower((unsigned char)*q);
    return s;
}

char *mi_lang_detect(const mi_html_node *doc, const char *content_language) {
    if (content_language && *content_language) {
        /* The header is authoritative over a missing attribute but not over the
         * page's own <html lang>; the page's own statement is checked first
         * below, so the header is used only if the page says nothing. */
    }
    if (doc) {
        for (const mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n)) {
            if (!mi_html_is(n, "html")) continue;
            const char *lang = mi_html_attr(n, "lang");
            if (!lang) lang = mi_html_attr(n, "xml:lang");
            if (lang && *lang) {
                char *p = primary_subtag(lang);
                if (p) return p;
            }
            break; /* only the first <html> */
        }
        /* An og:locale meta, which is often the only statement on a page that
         * omits <html lang>. */
        for (const mi_html_node *n = mi_html_next(doc, doc); n; n = mi_html_next(doc, n)) {
            if (!mi_html_is(n, "meta")) continue;
            const char *prop = mi_html_attr(n, "property");
            if (prop && !strcasecmp(prop, "og:locale")) {
                const char *v = mi_html_attr(n, "content");
                if (v && *v) {
                    char *p = primary_subtag(v);
                    if (p) return p;
                }
            }
        }
    }
    if (content_language && *content_language) {
        return primary_subtag(content_language);
    }
    return NULL;
}

/* Whether the text contains a character in a script only one language uses.
 * Decodes UTF-8, both the 2-byte range (Cyrillic, Hebrew, Arabic, Greek) and the
 * 3-byte range (CJK, Hangul) — the two a script test needs. */
static int has_script(const char *text, size_t len, int lo, int hi) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) continue;
        unsigned cp = 0;
        if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((unsigned)(c & 0x1F) << 6) | (unsigned)(text[i + 1] & 0x3F);
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((unsigned)(c & 0x0F) << 12) |
                 ((unsigned)(text[i + 1] & 0x3F) << 6) |
                 (unsigned)(text[i + 2] & 0x3F);
        } else {
            continue;
        }
        if ((int)cp >= lo && (int)cp <= hi) return 1;
    }
    return 0;
}

/* A crude count of how often a word appears as a whole word. */
static int count_word(const char *text, const char *word) {
    size_t wl = strlen(word);
    int n = 0;
    const char *p = text;
    while ((p = strcasestr(p, word)) != NULL) {
        char before = p > text ? p[-1] : ' ';
        char after = p[wl];
        if (!isalnum((unsigned char)before) && !isalnum((unsigned char)after)) n++;
        p += wl;
    }
    return n;
}

char *mi_lang_guess(const char *text, size_t len) {
    /* A script is decisive even in a few characters, so the guard is small. The
     * Latin word count below needs far more text than this to mean anything, and
     * guards itself with a threshold. */
    if (!text || len < 4) return NULL;

    /* Scripts first: they are decisive. */
    if (has_script(text, len, 0x3040, 0x30FF)) return mi_strdup("ja"); /* kana */
    if (has_script(text, len, 0xAC00, 0xD7AF)) return mi_strdup("ko"); /* hangul */
    if (has_script(text, len, 0x4E00, 0x9FFF)) return mi_strdup("zh"); /* CJK */
    if (has_script(text, len, 0x0400, 0x04FF)) return mi_strdup("ru"); /* cyrillic */
    if (has_script(text, len, 0x0600, 0x06FF)) return mi_strdup("ar"); /* arabic */
    if (has_script(text, len, 0x0370, 0x03FF)) return mi_strdup("el"); /* greek */
    if (has_script(text, len, 0x0590, 0x05FF)) return mi_strdup("he"); /* hebrew */
    if (has_script(text, len, 0x0E00, 0x0E7F)) return mi_strdup("th"); /* thai */

    /* Latin languages: the very common function words, counted. */
    struct { const char *code; const char *words[6]; } LANGS[] = {
        {"en", {"the", "and", "of", "to", "is", "that"}},
        {"fr", {"le", "la", "les", "des", "est", "et"}},
        {"de", {"der", "die", "das", "und", "ist", "nicht"}},
        {"es", {"el", "la", "los", "de", "es", "que"}},
        {"it", {"il", "la", "di", "che", "e", "per"}},
        {"pt", {"de", "que", "os", "para", "com", "nao"}},
        {"nl", {"de", "het", "een", "van", "en", "is"}},
    };
    const char *best = NULL;
    int best_score = 0;
    for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++) {
        int score = 0;
        for (int w = 0; w < 6; w++) score += count_word(text, LANGS[i].words[w]);
        if (score > best_score) { best_score = score; best = LANGS[i].code; }
    }
    /* A threshold: a page with a couple of "the"s and no other signal is not
     * confidently English. */
    if (best && best_score >= 4) return mi_strdup(best);
    return NULL;
}

int mi_lang_matches(const char *lang, const char *accept_list) {
    if (!accept_list || !*accept_list) return 1; /* no filter: keep everything */
    if (!lang || !*lang) return 0;               /* a filter and no language: drop */
    /* Compare primary subtags, case-insensitively. */
    size_t ll = strlen(lang);
    const char *p = accept_list;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == ';') {
            if (*p == ';') { /* skip a q= weight */
                while (*p && *p != ',') p++;
                continue;
            }
            p++;
        }
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ') p++;
        size_t n = (size_t)(p - start);
        if (n == ll && !strncasecmp(start, lang, ll)) return 1;
        /* A wildcard keeps everything. */
        if (n == 1 && *start == '*') return 1;
    }
    return 0;
}

char *mi_lang_accept_header(const char *preferences) {
    if (!preferences || !*preferences) return NULL;
    /*
     * The caller gives a plain list ("en,fr"); turn it into a q-weighted header
     * with the first preference highest, which is what a server expects when a
     * caller has an order rather than an equal set.
     */
    mi_json_buf b;
    mi_json_init(&b);
    int first = 1;
    double q = 1.0;
    const char *p = preferences;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ') p++;
        if (!first) mi_json_raw(&b, ",");
        first = 0;
        char buf[64];
        size_t n = (size_t)(p - start);
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, start, n);
        buf[n] = 0;
        mi_json_raw(&b, buf);
        if (q < 1.0) {
            char qs[16];
            snprintf(qs, sizeof qs, ";q=%.1f", q);
            mi_json_raw(&b, qs);
        }
        q -= 0.1;
        if (q < 0.1) q = 0.1;
    }
    char *out = mi_strdup(mi_json_data(&b));
    mi_json_free(&b);
    return out;
}
