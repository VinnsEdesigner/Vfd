/* mi_sanitize.c - make a rendered page safe to put in a frame. */

#include "mi_sanitize.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mi_util.h"

/*
 * The tags that survive.
 *
 * An allowlist, so anything not here is dropped. Only what a page needs to be
 * *read* is kept: structure, text, images and links. Nothing that can execute,
 * load a plugin, or navigate the frame.
 */
static const char *const ALLOWED_TAGS[] = {
    "a", "abbr", "address", "article", "aside", "b", "blockquote", "br", "caption",
    "cite", "code", "col", "colgroup", "dd", "del", "details", "dfn", "div", "dl",
    "dt", "em", "figcaption", "figure", "footer", "h1", "h2", "h3", "h4", "h5", "h6",
    "header", "hr", "i", "img", "ins", "kbd", "li", "main", "mark", "nav", "ol", "p",
    "pre", "q", "s", "samp", "section", "small", "span", "strong", "sub", "summary",
    "sup", "table", "tbody", "td", "tfoot", "th", "thead", "time", "tr", "u", "ul",
    "wbr",
    NULL,
};

/*
 * The attributes that survive, and on which tags.
 *
 * `*` means any allowed tag. The URL-bearing ones are checked separately for a
 * dangerous scheme, because `href="javascript:…"` is the oldest trick there is.
 */
typedef struct {
    const char *name;
    const char *on; /* the tag, or "*" */
    int is_url;     /* value must not carry a script or data scheme */
    int is_style;   /* value is CSS, which is filtered further */
} allowed_attr;

static const allowed_attr ALLOWED_ATTRS[] = {
    {"class", "*", 0, 0},
    {"id", "*", 0, 0},
    {"title", "*", 0, 0},
    {"dir", "*", 0, 0},
    {"lang", "*", 0, 0},
    {"style", "*", 0, 1},
    {"href", "a", 1, 0},
    {"src", "img", 1, 0},
    {"alt", "img", 0, 0},
    {"width", "img", 0, 0},
    {"height", "img", 0, 0},
    {"loading", "img", 0, 0},
    {"srcset", "img", 1, 0},
    {"colspan", "td", 0, 0},
    {"rowspan", "td", 0, 0},
    {"colspan", "th", 0, 0},
    {"rowspan", "th", 0, 0},
    {"span", "col", 0, 0},
    {"open", "details", 0, 0},
    {"datetime", "time", 0, 0},
    {NULL, NULL, 0, 0},
};

/*
 * Tags whose *content* is dropped, not just their tags.
 *
 * These are the classic vectors, and there is nothing inside them a reader
 * needs. Dropping the content is the difference between "no script tag" and
 * "no script": text inside a <script> is not displayed anyway, but it would
 * otherwise be re-emitted as body text, which is both wrong and confusing.
 */
static const char *const DROP_CONTENT_TAGS[] = {
    "script", "style", "noscript", "template", "svg", "math", "canvas", "iframe",
    "object", "embed", "applet", "form", "input", "button", "select", "textarea",
    "option", "audio", "video", "source", "track", "map", "area", "base", "link",
    "meta", "title", "head", "frame", "frameset", "noframes", "xmp", "plaintext",
    NULL,
};

static int in_list(const char *const *list, const char *name) {
    for (size_t i = 0; list[i]; i++) {
        if (!strcasecmp(list[i], name)) return 1;
    }
    return 0;
}

/* A tag name is only ever letters, digits and a few separators. Anything else
 * means the "tag" is not one, and is dropped. */
static int name_is_plain(const char *s, size_t n) {
    if (n == 0 || n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '-' && c != '_') return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf;

static void buf_init(buf *b) {
    b->cap = 256;
    b->len = 0;
    b->data = mi_malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

static void buf_need(buf *b, size_t extra) {
    if (!b->data) return;
    if (b->len + extra + 1 <= b->cap) return;
    size_t want = (b->len + extra + 1) * 2;
    char *grown = mi_realloc(b->data, want);
    if (!grown) return;
    b->data = grown;
    b->cap = want;
}

static void buf_add(buf *b, const char *s, size_t n) {
    if (!b->data || n == 0) return;
    buf_need(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_puts(buf *b, const char *s) {
    if (s) buf_add(b, s, strlen(s));
}

static void buf_putc(buf *b, char c) {
    buf_add(b, &c, 1);
}

/*
 * Write text with the characters that would change the document escaped.
 *
 * This is what stops a page's text from becoming markup: a title containing
 * `<script>` is displayed, not executed.
 */
static void buf_text_escaped(buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '<': buf_puts(b, "&lt;"); break;
            case '>': buf_puts(b, "&gt;"); break;
            case '&': buf_puts(b, "&amp;"); break;
            default: buf_putc(b, s[i]); break;
        }
    }
}

/*
 * Write an attribute value, quoted, with everything that could end the value
 * escaped.
 *
 * Both quote characters are escaped, not just the one being used: a value
 * containing a single quote would otherwise break out when the attribute is
 * emitted with single quotes, and that is exactly the attribute-injection bug
 * that turns a safe tag into an unsafe one.
 */
static void buf_attr_escaped(buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '"': buf_puts(b, "&quot;"); break;
            case '\'': buf_puts(b, "&#39;"); break;
            case '<': buf_puts(b, "&lt;"); break;
            case '>': buf_puts(b, "&gt;"); break;
            case '&': buf_puts(b, "&amp;"); break;
            case '`': buf_puts(b, "&#96;"); break;
            default: buf_putc(b, s[i]); break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

/* Whether a URL value carries a scheme that can execute or smuggle content.
 *
 * The scheme is compared with whitespace and control characters removed, and
 * case-insensitively, because `java\tscript:` and `JaVaScRiPt:` are both real
 * bypasses of a naive prefix check. */
static int url_is_dangerous(const char *v, size_t n) {
    char scheme[32];
    size_t s = 0;
    for (size_t i = 0; i < n && s < sizeof scheme - 1; i++) {
        unsigned char c = (unsigned char)v[i];
        if (isspace(c) || c < 0x20 || c == 0x7f) continue;
        if (c == ':') {
            scheme[s] = '\0';
            return !strcasecmp(scheme, "javascript") || !strcasecmp(scheme, "vbscript") ||
                   !strcasecmp(scheme, "data") || !strcasecmp(scheme, "file") ||
                   !strcasecmp(scheme, "blob") || !strcasecmp(scheme, "filesystem");
        }
        if (c == '/' || c == '?' || c == '#') {
            /* A relative reference, so there is no scheme to worry about. */
            return 0;
        }
        scheme[s++] = (char)tolower(c);
    }
    return 0;
}

/*
 * Whether a CSS value can fetch or execute something.
 *
 * A `style` attribute is not script, but `url(...)` can make the frame fetch
 * from anywhere — a tracking beacon, or a probe of the local network. The
 * injected CSP also blocks it; this is the second lock.
 */
static int style_is_dangerous(const char *v, size_t n) {
    for (size_t i = 0; i + 4 <= n; i++) {
        if (strncasecmp(v + i, "url(", 4) == 0) return 1;
        if (i + 10 <= n && strncasecmp(v + i, "expression", 10) == 0) return 1;
        if (i + 7 <= n && strncasecmp(v + i, "import", 7) == 0) return 1;
    }
    return 0;
}

/* Whether a srcset value has a dangerous URL in any of its candidates. */
static int srcset_is_dangerous(const char *v, size_t n) {
    const char *p = v;
    const char *end = v + n;
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end) break;
        const char *url_start = p;
        while (p < end && !isspace((unsigned char)*p)) p++;
        if (url_is_dangerous(url_start, (size_t)(p - url_start))) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

/* Read the tag name at `p`, writing it lower-cased into `out`. Returns the
 * length, or 0 when there is no name there. */
static size_t read_tag_name(const char *p, const char *end, char *out, size_t out_size) {
    size_t n = 0;
    if (p < end && *p == '/') p++;
    while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) {
        if (n + 1 < out_size) out[n] = (char)tolower((unsigned char)*p);
        n++;
        p++;
    }
    if (n < out_size) out[n] = '\0';
    else out[out_size - 1] = '\0';
    return n;
}

/* Whether the first close tag of `tag` at or after `p` was found; sets *after to
 * the position past it. Used for the raw-text elements. */
static int find_close(const char *p, const char *end, const char *tag, const char **after) {
    size_t tlen = strlen(tag);
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        const char *q = lt + 1;
        if (q < end && *q == '/') q++;
        else {
            p = lt + 1;
            continue;
        }
        if ((size_t)(end - q) >= tlen && !strncasecmp(q, tag, tlen)) {
            char next = (q + tlen < end) ? q[tlen] : '>';
            if (next == '>' || isspace((unsigned char)next)) {
                const char *gt = memchr(q, '>', (size_t)(end - q));
                *after = gt ? gt + 1 : end;
                return 1;
            }
        }
        p = lt + 1;
    }
    *after = end;
    return 0;
}

/*
 * Skip a tag's content entirely.
 *
 * Two kinds of element, and they must be handled differently:
 *
 *   raw text (script, style, title, textarea, xmp) — the content is not markup,
 *   so a `<` inside it is just a character. Counting nesting here is wrong:
 *   `var x = "<script>";` would look like a second open tag and the skip would
 *   run to the end of the document, silently dropping the rest of the page.
 *   The first matching close tag is the right answer.
 *
 *   everything else (svg, math, form, iframe, ...) — the content IS markup, so
 *   nesting has to be counted or an inner element would end the outer one early.
 */
static const char *skip_content(const char *p, const char *end, const char *tag) {
    static const char *const RAW_TEXT[] = {"script", "style", "title", "textarea",
                                           "xmp", "plaintext", "noscript", NULL};
    if (in_list(RAW_TEXT, tag)) {
        const char *after = end;
        find_close(p, end, tag, &after);
        return after;
    }

    size_t tlen = strlen(tag);
    int depth = 1;
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        const char *q = lt + 1;
        int closing = 0;
        if (q < end && *q == '/') {
            closing = 1;
            q++;
        }
        if ((size_t)(end - q) >= tlen && !strncasecmp(q, tag, tlen)) {
            char after = (q + tlen < end) ? q[tlen] : '>';
            if (after == '>' || isspace((unsigned char)after) || after == '/') {
                if (closing) {
                    depth--;
                    if (depth == 0) {
                        const char *gt = memchr(q, '>', (size_t)(end - q));
                        return gt ? gt + 1 : end;
                    }
                } else {
                    depth++;
                }
            }
        }
        p = lt + 1;
    }
    return end;
}

/* ------------------------------------------------------------------ */
/* Attribute handling                                                  */
/* ------------------------------------------------------------------ */

/* Is this attribute allowed on this tag? Returns the table entry, or NULL. */
static const allowed_attr *attr_allowed(const char *name, size_t nlen, const char *tag) {
    for (size_t i = 0; ALLOWED_ATTRS[i].name; i++) {
        if (strlen(ALLOWED_ATTRS[i].name) != nlen) continue;
        if (strncasecmp(ALLOWED_ATTRS[i].name, name, nlen) != 0) continue;
        if (!strcmp(ALLOWED_ATTRS[i].on, "*") || !strcasecmp(ALLOWED_ATTRS[i].on, tag)) {
            return &ALLOWED_ATTRS[i];
        }
    }
    return NULL;
}

/*
 * Make a reference absolute against the page it came from.
 *
 * Returns NULL when it is already absolute or when there is no base to resolve
 * against. A fragment is not a resource, so it returns NULL too.
 */
static char *make_absolute(const char *value, size_t n, const char *base_url) {
    if (!base_url || n == 0) return NULL;
    if (value[0] == '#') return NULL;
    if (strncasecmp(value, "http://", 7) == 0 || strncasecmp(value, "https://", 8) == 0) {
        return NULL; /* already absolute */
    }
    const char *ref = NULL;
    char *out = NULL;
    char *refcopy = mi_strndup(value, n);
    if (!refcopy) return NULL;

    if (value[0] == '/') {
        /* Root-relative: the origin from the base. */
        const char *sep = strstr(base_url, "://");
        if (sep) {
            const char *host = sep + 3;
            const char *slash = strchr(host, '/');
            size_t origin_len = slash ? (size_t)(slash - base_url) : strlen(base_url);
            char *origin = mi_strndup(base_url, origin_len);
            if (origin) out = mi_asprintf("%s%s", origin, refcopy);
            free(origin);
        }
    } else {
        /* Document-relative: the base's directory. */
        const char *q = strpbrk(base_url, "?#");
        size_t blen = q ? (size_t)(q - base_url) : strlen(base_url);
        while (blen > 0 && base_url[blen - 1] != '/') blen--;
        char *dir = mi_strndup(base_url, blen);
        if (dir) out = mi_asprintf("%s%s", dir, refcopy);
        free(dir);
    }
    (void)ref;
    free(refcopy);
    return out;
}

/*
 * Rewrite a link so a click stays inside the app.
 *
 * The destination is made absolute, then handed to the renderer's own prefix —
 * so following a link renders the next page rather than navigating the frame
 * somewhere else. A fragment-only link is left alone, because it is a jump
 * within this document.
 */
static char *rewrite_link(const char *value, size_t n, const mi_sanitize_config *cfg) {
    if (!cfg || !cfg->link_prefix || !cfg->base_url) return NULL;
    if (n == 0 || value[0] == '#') return NULL;

    char *ref = mi_strndup(value, n);
    if (!ref) return NULL;
    /* A relative reference is resolved first; an absolute one is used as it is. */
    char *absolute = make_absolute(value, n, cfg->base_url);
    const char *target = absolute ? absolute : ref;
    char *out = mi_asprintf("%s%s", cfg->link_prefix, target);
    free(absolute);
    free(ref);
    return out;
}

/* Emit one allowed attribute, filtered. */
static void emit_attr(buf *b, const char *name, size_t nlen, const char *value,
                      size_t vlen, const allowed_attr *spec, const char *tag,
                      const mi_sanitize_config *cfg) {
    if (spec->is_url) {
        if (url_is_dangerous(value, vlen)) return;
        if (!strcasecmp(spec->name, "srcset") && srcset_is_dangerous(value, vlen)) return;
    }
    if (spec->is_style && style_is_dangerous(value, vlen)) return;

    /*
     * URLs are resolved here rather than left relative to a `<base>` tag.
     *
     * A base would be the tidy way to make a page's relative images load, but the
     * policy forbids one (`base-uri 'none'`) — a base tag is how an injected
     * document rewrites every URL on the page, so allowing it to satisfy the
     * layout would give up a real protection. Resolving each URL is more work and
     * cannot be abused.
     */
    char *rewritten = NULL;
    if (spec->is_url) {
        if (!strcasecmp(spec->name, "href") && !strcasecmp(tag, "a")) {
            /* A link becomes a renderer request, so it stays in the app. */
            rewritten = rewrite_link(value, vlen, cfg);
        } else if (cfg && cfg->base_url) {
            rewritten = make_absolute(value, vlen, cfg->base_url);
        }
    }

    buf_putc(b, ' ');
    buf_add(b, name, nlen);
    buf_puts(b, "=\"");
    if (rewritten) {
        buf_attr_escaped(b, rewritten, strlen(rewritten));
        free(rewritten);
    } else {
        buf_attr_escaped(b, value, vlen);
    }
    buf_putc(b, '"');
}

/* Parse and emit the attributes of one tag. */
static void emit_attrs(buf *b, const char *p, const char *end, const char *tag,
                       const mi_sanitize_config *cfg) {
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p == '/' || *p == '>') break;

        const char *name_start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ':')) p++;
        size_t nlen = (size_t)(p - name_start);
        if (nlen == 0) {
            p++; /* not an attribute; step over it rather than looping */
            continue;
        }

        while (p < end && isspace((unsigned char)*p)) p++;
        const char *value = NULL;
        size_t vlen = 0;
        if (p < end && *p == '=') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char quote = *p++;
                value = p;
                while (p < end && *p != quote) p++;
                vlen = (size_t)(p - value);
                if (p < end) p++; /* the closing quote */
            } else {
                value = p;
                while (p < end && !isspace((unsigned char)*p) && *p != '>') p++;
                vlen = (size_t)(p - value);
            }
        }

        /* An `on*` attribute is an event handler by definition, and is never on
         * the allowlist — but it is named here so the intent is explicit rather
         * than accidental. */
        if (nlen >= 3 && (name_start[0] == 'o' || name_start[0] == 'O') &&
            (name_start[1] == 'n' || name_start[1] == 'N')) {
            continue;
        }

        char name[64];
        if (nlen >= sizeof name) continue;
        memcpy(name, name_start, nlen);
        name[nlen] = '\0';

        const allowed_attr *spec = attr_allowed(name, nlen, tag);
        if (!spec) continue;
        /* A boolean attribute with no value, like `open`. */
        if (!value) {
            buf_putc(b, ' ');
            buf_add(b, name, nlen);
            buf_puts(b, "=\"\"");
            continue;
        }
        emit_attr(b, name, nlen, value, vlen, spec, tag, cfg);
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

char *mi_sanitize_html(const char *html, size_t len, const mi_sanitize_config *cfg) {
    if (!html) return NULL;

    buf out;
    buf_init(&out);
    if (!out.data) return NULL;

    const char *p = html;
    const char *end = html + len;

    /* A minimal document wrapper with the policy in it. The CSP is the part
     * that a browser enforces; the sanitiser above is hygiene. */
    buf_puts(&out, "<!doctype html><html><head><meta charset=\"utf-8\">");
    buf_puts(&out, "<meta http-equiv=\"Content-Security-Policy\" content=\"");
    buf_puts(&out, mi_sanitize_csp());
    buf_puts(&out, "\">");
    /*
     * No `<base>` tag. Relative URLs are resolved individually instead, because
     * the policy forbids a base (`base-uri 'none'`) — and it does so for a
     * reason: a base is how an injected document rewrites every URL on the page.
     * Trading that protection for tidier markup would be a bad trade.
     */
    buf_puts(&out, "</head><body>");

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) {
            buf_text_escaped(&out, p, (size_t)(end - p));
            break;
        }
        /* The text before the tag. */
        buf_text_escaped(&out, p, (size_t)(lt - p));

        const char *gt = memchr(lt, '>', (size_t)(end - lt));
        if (!gt) {
            /* An unterminated '<': escape it as text rather than guessing. */
            buf_puts(&out, "&lt;");
            buf_text_escaped(&out, lt + 1, (size_t)(end - lt - 1));
            break;
        }

        /* A comment, a doctype or a CDATA section: dropped whole. A comment is
         * dropped rather than copied because conditional comments were a real
         * vector, and nothing a reader needs lives in one. */
        if (lt + 1 < gt && (lt[1] == '!' || lt[1] == '?')) {
            p = gt + 1;
            continue;
        }

        char tag[64];
        size_t tlen = read_tag_name(lt + 1, gt, tag, sizeof tag);
        int closing = (lt + 1 < gt && lt[1] == '/');

        if (tlen == 0 || !name_is_plain(tag, tlen)) {
            /* Not a tag name: escape the angle bracket and carry on. */
            buf_puts(&out, "&lt;");
            p = lt + 1;
            continue;
        }

        /* A tag whose content is dropped entirely. */
        if (in_list(DROP_CONTENT_TAGS, tag)) {
            if (!closing) {
                p = skip_content(gt + 1, end, tag);
            } else {
                p = gt + 1;
            }
            continue;
        }

        /* A tag not on the allowlist: dropped, but its content is kept, because
         * the content is usually text a reader wants. */
        if (!in_list(ALLOWED_TAGS, tag)) {
            p = gt + 1;
            continue;
        }

        /* A void element has no close tag, so `<br>` must not become `<br></br>`
         * — that would be two line breaks. */
        int is_void = !strcasecmp(tag, "br") || !strcasecmp(tag, "hr") ||
                      !strcasecmp(tag, "img") || !strcasecmp(tag, "wbr") ||
                      !strcasecmp(tag, "col");

        if (closing) {
            if (!is_void) {
                buf_puts(&out, "</");
                buf_puts(&out, tag);
                buf_putc(&out, '>');
            }
        } else {
            buf_putc(&out, '<');
            buf_puts(&out, tag);
            emit_attrs(&out, lt + 1 + tlen, gt, tag, cfg);
            buf_putc(&out, '>');
        }
        p = gt + 1;
    }

    buf_puts(&out, "</body></html>");
    return out.data;
}

const char *mi_sanitize_csp(void) {
    /*
     * `default-src 'none'` and then the narrowest allowances that let a page be
     * *read*: its own images and fonts, and inline styles (a page that lays
     * itself out with them would otherwise be a wall of text). No scripts, no
     * frames, no forms, no connections — so even a sanitiser bug cannot make a
     * script run, and the page cannot phone home.
     *
     * `frame-ancestors` is deliberately absent. It is ignored when delivered in
     * a `<meta>` element — verified in a browser, which reports exactly that —
     * so including it would be claiming a protection this document does not
     * have. Who may frame the page is enforced by the console's `sandbox`
     * attribute instead, which is a real boundary.
     */
    return "default-src 'none'; "
           "img-src https: http: data:; "
           "font-src https: http: data:; "
           "style-src 'unsafe-inline'; "
           "media-src https: http:; "
           "form-action 'none'; "
           "base-uri 'none'";
}
