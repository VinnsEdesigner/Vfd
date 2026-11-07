/* mi_browse.c - list the media a remote page or directory offers. */

#include "mi_browse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mi_download.h"
#include "mi_extract.h"
#include "mi_json.h"
#include "mi_util.h"

/* Percent-decode a path segment, so a file called "cat%20(1).jpg" is offered as
 * "cat (1).jpg" — the name a person recognises and a save should use. */
static char *url_decode(const char *s) {
    size_t n = strlen(s);
    char *out = mi_malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = isxdigit((unsigned char)s[i + 1]) ? s[i + 1] : -1;
            int lo = isxdigit((unsigned char)s[i + 2]) ? s[i + 2] : -1;
            if (hi >= 0 && lo >= 0) {
                char buf[3] = {(char)s[i + 1], (char)s[i + 2], '\0'};
                out[o++] = (char)strtol(buf, NULL, 16);
                i += 2;
                continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = '\0';
    return out;
}

/*
 * A listing is capped: a page with a thousand links would otherwise turn into a
 * thousand cards nobody asked for, and each one is a URL the console renders.
 *
 * The cap has to be generous enough to survive a modern page, though. A page
 * like a GitHub repository carries several hundred script and style references
 * in its <head> *before* any content link appears — measured: the first file
 * link on github.com/torvalds/linux is at character 218,964, past a 200-entry
 * cap. A limit that stops before the content is not a limit, it is a blind
 * spot, so the cap is set where it still bounds memory without hiding the page.
 */
#define MAX_ITEMS 2000
/* Bodies are capped too; a huge page is not worth buffering to read a list. */
#define MAX_BODY (2 * 1024 * 1024)
/* A URL longer than this is not a media reference worth carrying. */
#define MAX_URL 2048

/* ------------------------------------------------------------------ */
/* URL handling                                                        */
/* ------------------------------------------------------------------ */

/* Case-insensitive prefix test. */
static int starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++;
        prefix++;
    }
    return 1;
}

/* The scheme and authority of a URL, e.g. "https://host:8080". Allocated, or
 * NULL when the URL is not absolute. */
static char *url_origin(const char *url) {
    const char *sep = strstr(url, "://");
    if (!sep) return NULL;
    const char *host = sep + 3;
    const char *end = host;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    return mi_strndup(url, (size_t)(end - url));
}

/* Everything up to and including the last '/', i.e. the directory part. */
static char *url_dir(const char *url) {
    const char *q = strpbrk(url, "?#");
    size_t len = q ? (size_t)(q - url) : strlen(url);
    while (len > 0 && url[len - 1] != '/') len--;
    return mi_strndup(url, len);
}

/*
 * Resolve a reference against the page it came from, the way a browser does.
 * Handles absolute URLs, scheme-relative ("//host/x"), root-relative ("/x"),
 * document-relative ("x/y") and "./" / "../" segments. Returns NULL when the
 * result would not be an absolute http(s) URL.
 */
static char *resolve_url(const char *base, const char *ref) {
    if (!ref || !*ref) return NULL;
    /* A fragment is not a resource. */
    if (ref[0] == '#') return NULL;
    /* Not something we can fetch. */
    if (starts_with_ci(ref, "data:") || starts_with_ci(ref, "javascript:") ||
        starts_with_ci(ref, "mailto:") || starts_with_ci(ref, "blob:")) {
        return NULL;
    }

    if (starts_with_ci(ref, "http://") || starts_with_ci(ref, "https://")) {
        char *out = mi_strdup(ref);
        return out;
    }

    char *origin = url_origin(base);
    if (!origin) return NULL;

    if (ref[0] == '/' && ref[1] == '/') {
        /* Scheme-relative: borrow the base's scheme. */
        char *out = mi_asprintf("%s%s", starts_with_ci(base, "https") ? "https:" : "http:", ref);
        free(origin);
        return out;
    }

    char *joined = NULL;
    if (ref[0] == '/') {
        joined = mi_asprintf("%s%s", origin, ref);
    } else {
        char *dir = url_dir(base);
        if (!dir) {
            free(origin);
            return NULL;
        }
        joined = mi_asprintf("%s%s", dir, ref);
        free(dir);
    }
    free(origin);

    /* Collapse "." and ".." segments so the URL we hand on is the one a browser
     * would actually request, and so a listing cannot walk upwards forever. */
    char *scheme_end = strstr(joined, "://");
    if (!scheme_end) {
        free(joined);
        return NULL;
    }
    const char *path_start = scheme_end + 3;
    while (*path_start && *path_start != '/') path_start++;
    if (!*path_start) return joined;

    size_t head_len = (size_t)(path_start - joined);
    char *out = mi_malloc(head_len + strlen(path_start) + 2);
    if (!out) {
        free(joined);
        return NULL;
    }
    memcpy(out, joined, head_len);
    size_t o = head_len;
    out[o++] = '/';

    const char *seg = path_start + 1;
    while (*seg) {
        const char *end = strchr(seg, '/');
        size_t n = end ? (size_t)(end - seg) : strlen(seg);
        if (n == 0) {
            /* skip an empty segment */
        } else if (n == 1 && seg[0] == '.') {
            /* skip */
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            /* pop one segment, never above the root */
            if (o > head_len + 1) {
                o--; /* the trailing slash */
                while (o > head_len + 1 && out[o - 1] != '/') o--;
            }
        } else {
            memcpy(out + o, seg, n);
            o += n;
            if (end) out[o++] = '/';
        }
        if (!end) break;
        seg = end + 1;
    }
    out[o] = '\0';
    free(joined);
    return out;
}

/* The last path segment, percent-decoded, query and fragment stripped. */
static char *basename_of(const char *url) {
    const char *q = strpbrk(url, "?#");
    size_t len = q ? (size_t)(q - url) : strlen(url);
    const char *end = url + len;
    const char *start = end;
    while (start > url && start[-1] != '/') start--;
    if (start == end) return NULL;

    char *raw = mi_strndup(start, (size_t)(end - start));
    if (!raw) return NULL;
    char *decoded = url_decode(raw);
    free(raw);
    return decoded;
}

/* ------------------------------------------------------------------ */
/* Content types                                                       */
/* ------------------------------------------------------------------ */

/* A content type guessed from the extension. Empty when unknown: Vyzor
 * sniffs the real bytes at download time, so this is only a hint for the card. */
static const char *mime_for_name(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "";
    const char *ext = dot + 1;

    struct { const char *ext; const char *mime; } TABLE[] = {
        {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"mov", "video/quicktime"},
        {"webm", "video/webm"}, {"mkv", "video/x-matroska"}, {"avi", "video/x-msvideo"},
        {"flv", "video/x-flv"}, {"ogv", "video/ogg"}, {"ts", "video/mp2t"},
        {"m3u8", "application/vnd.apple.mpegurl"}, {"mpd", "application/dash+xml"},
        {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"aac", "audio/aac"},
        {"flac", "audio/flac"}, {"wav", "audio/wav"}, {"oga", "audio/ogg"},
        {"ogg", "audio/ogg"}, {"opus", "audio/opus"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"},
        {"gif", "image/gif"}, {"webp", "image/webp"}, {"bmp", "image/bmp"},
        {"tiff", "image/tiff"}, {"avif", "image/avif"}, {"heic", "image/heic"},
        {"svg", "image/svg+xml"}, {"pdf", "application/pdf"},
        {"txt", "text/plain"}, {"md", "text/markdown"}, {"json", "application/json"},
        {"csv", "text/csv"}, {"xml", "application/xml"}, {"srt", "application/x-subrip"},
        {"vtt", "text/vtt"},
    };
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (!strcasecmp(ext, TABLE[i].ext)) return TABLE[i].mime;
    }
    return "";
}

/* Whether a name looks like something this app is for. Kept in step with the
 * extensions above: a listing should not fill up with stylesheets and scripts. */
static int is_media_name(const char *name) {
    return mime_for_name(name)[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Collection                                                          */
/* ------------------------------------------------------------------ */

static void result_init(mi_browse_result *out) {
    out->items = NULL;
    out->count = 0;
    out->title = NULL;
}

void mi_browse_result_free(mi_browse_result *res) {
    if (!res) return;
    for (size_t i = 0; i < res->count; i++) {
        free(res->items[i].url);
        free(res->items[i].filename);
        free(res->items[i].title);
        free(res->items[i].mime);
        free(res->items[i].thumbnail);
    }
    free(res->items);
    free(res->title);
    memset(res, 0, sizeof *res);
}

/* Whether this URL is already in the list. A page often references the same
 * file several times (a link, a poster, a srcset entry). */
static int already_have(const mi_browse_result *out, const char *url) {
    for (size_t i = 0; i < out->count; i++) {
        if (!strcmp(out->items[i].url, url)) return 1;
    }
    return 0;
}

/*
 * Offer a reference for the list. `base` resolves it; `size_hint` is -1 when the
 * source did not say; `title` is the link text, when there was one.
 *
 * Under MI_BROWSE_MEDIA anything that is not a media-looking URL is dropped, so
 * the caller can hand over raw attributes without pre-filtering. Under
 * MI_BROWSE_ALL everything is kept, which is what makes a listing read like the
 * page does.
 */
static void offer(mi_browse_result *out, const char *base, const char *ref,
                  const char *title, int64_t size_hint, mi_browse_filter filter) {
    if (out->count >= MAX_ITEMS) return;
    if (!ref || strlen(ref) > MAX_URL) return;

    char *abs = resolve_url(base, ref);
    if (!abs) return;
    if (!starts_with_ci(abs, "http://") && !starts_with_ci(abs, "https://")) {
        free(abs);
        return;
    }

    char *name = basename_of(abs);
    if (!name || !*name) {
        /* No final segment: a directory or the site root. There is nothing to
         * name the file after, so media mode drops it. Everything mode keeps it
         * — it is a real place to go — under a placeholder that says so. */
        if (filter != MI_BROWSE_ALL) {
            free(name);
            free(abs);
            return;
        }
        free(name);
        name = mi_strdup("index");
    }
    if (filter != MI_BROWSE_ALL && !is_media_name(name)) {
        free(name);
        free(abs);
        return;
    }
    if (already_have(out, abs)) {
        free(name);
        free(abs);
        return;
    }

    mi_browse_item *grown = mi_realloc(out->items, (out->count + 1) * sizeof *grown);
    if (!grown) {
        free(name);
        free(abs);
        return;
    }
    out->items = grown;
    mi_browse_item *it = &out->items[out->count++];
    it->url = abs;
    it->filename = name;
    it->title = title && *title ? mi_strdup(title) : NULL;
    it->mime = mi_strdup(mime_for_name(name));
    it->bytes = size_hint;
    /* The realloc'd slot is uninitialised, so every field the struct has must be
     * set here — a field left out is a garbage pointer the JSON writer follows. */
    it->resolved = 0;
    it->thumbnail = NULL;
}

/* ------------------------------------------------------------------ */
/* HTML                                                                */
/* ------------------------------------------------------------------ */

/* Case-insensitive search for `needle` from `from`. */
static const char *find_ci(const char *from, const char *needle) {
    if (!from || !*from) return NULL;
    size_t nlen = strlen(needle);
    for (const char *p = from; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

/* The value of an attribute in a tag body, allocated. Handles single and double
 * quotes and unquoted values, which is what real pages use. */
static char *attr_value(const char *tag, size_t tag_len, const char *name) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen < tag_len; i++) {
        /* The name must start at a boundary, or "src" would match "data-src". */
        if (i > 0) {
            char prev = tag[i - 1];
            if (isalnum((unsigned char)prev) || prev == '-' || prev == '_') continue;
        }
        if (strncasecmp(tag + i, name, nlen) != 0) continue;
        size_t j = i + nlen;
        while (j < tag_len && isspace((unsigned char)tag[j])) j++;
        if (j >= tag_len || tag[j] != '=') continue;
        j++;
        while (j < tag_len && isspace((unsigned char)tag[j])) j++;
        if (j >= tag_len) return NULL;

        char quote = tag[j];
        if (quote == '"' || quote == '\'') {
            j++;
            size_t start = j;
            while (j < tag_len && tag[j] != quote) j++;
            return mi_strndup(tag + start, j - start);
        }
        size_t start = j;
        while (j < tag_len && !isspace((unsigned char)tag[j]) && tag[j] != '>') j++;
        return mi_strndup(tag + start, j - start);
    }
    return NULL;
}

/* Strip HTML entities so a title reads as text rather than "Fish &amp; Chips". */
static void decode_entities_inplace(char *s) {
    struct { const char *ent; char ch; } TABLE[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '},
    };
    char *w = s;
    for (char *r = s; *r;) {
        int matched = 0;
        for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
            size_t n = strlen(TABLE[i].ent);
            if (!strncasecmp(r, TABLE[i].ent, n)) {
                *w++ = TABLE[i].ch;
                r += n;
                matched = 1;
                break;
            }
        }
        if (!matched) *w++ = *r++;
    }
    *w = '\0';
}

/* The page's <title>, trimmed and entity-decoded. Allocated or NULL. */
static char *extract_title(const char *body, size_t len) {
    /* Find the tags on a lowercased copy, but take the *text* from the original:
       the offsets are only meaningful against one of the two buffers, so the
       text is sliced out before the copy is released. */
    char *lower = mi_strndup(body, len);
    if (!lower) return NULL;
    for (size_t i = 0; i < len; i++) lower[i] = (char)tolower((unsigned char)lower[i]);

    const char *open = strstr(lower, "<title");
    const char *gt = open ? strchr(open, '>') : NULL;
    const char *close = gt ? find_ci(gt + 1, "</title>") : NULL;

    char *title = NULL;
    if (gt && close) {
        size_t start = (size_t)(gt + 1 - lower);
        size_t n = (size_t)(close - (gt + 1));
        title = mi_strndup(body + start, n);
    }
    free(lower);
    if (!title) return NULL;

    /* Collapse runs of whitespace, which a title tag is full of. */
    char *w = title;
    int space = 0;
    for (char *r = title; *r; r++) {
        if (isspace((unsigned char)*r)) {
            space = 1;
            continue;
        }
        if (space && w != title) *w++ = ' ';
        space = 0;
        *w++ = *r;
    }
    *w = '\0';
    decode_entities_inplace(title);
    return title;
}

/* The text between a tag's `>` and its matching close tag, as a person would
 * read it. Only used for anchors, where it is the name of the destination.
 * Allocated, or NULL when there is nothing usable. */
static char *link_text(const char *from, const char *end, const char *tagname) {
    size_t nlen = strlen(tagname);
    size_t close_len = nlen + 3; /* "</a>" */
    /* Find the matching close tag, so a nested element does not cut it short. */
    const char *close = NULL;
    for (const char *q = from; q < end; q++) {
        if (*q != '<') continue;
        if (q + close_len <= end && q[1] == '/' && !strncasecmp(q + 2, tagname, nlen)) {
            close = q;
            break;
        }
        /* Another opening tag of the same kind means the first was not closed;
         * stop rather than swallowing the rest of the page. */
        if (q + 1 + nlen <= end && !strncasecmp(q + 1, tagname, nlen)) break;
    }
    if (!close || close <= from) return NULL;

    /* Strip any nested markup, then collapse whitespace and decode entities. */
    char *raw = mi_strndup(from, (size_t)(close - from));
    if (!raw) return NULL;
    char *w = raw;
    int in_tag = 0;
    for (char *r = raw; *r; r++) {
        if (*r == '<') {
            in_tag = 1;
            continue;
        }
        if (*r == '>') {
            in_tag = 0;
            continue;
        }
        if (!in_tag) *w++ = *r;
    }
    *w = '\0';

    char *trimmed = raw;
    while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
    char *out = mi_strdup(trimmed);
    free(raw);
    if (!out) return NULL;
    w = out;
    int space = 0;
    for (char *r = out; *r; r++) {
        if (isspace((unsigned char)*r)) {
            space = 1;
            continue;
        }
        if (space && w != out) *w++ = ' ';
        space = 0;
        *w++ = *r;
    }
    *w = '\0';
    decode_entities_inplace(out);
    /* Cap it: link text is a label, not a paragraph. */
    if (strlen(out) > 120) out[120] = '\0';
    if (!*out) {
        free(out);
        return NULL;
    }
    return out;
}

/* Walk the tags and pull the references out of the attributes that carry them. */
static void parse_html(const char *body, size_t len, const char *base,
                       mi_browse_filter filter, mi_browse_result *out) {
    const char *p = body;
    const char *end = body + len;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) break;
        const char *gt = memchr(lt, '>', (size_t)(end - lt));
        if (!gt) break;

        const char *tag = lt + 1;
        size_t tag_len = (size_t)(gt - tag);

        /* A tag name, for deciding which attributes matter. */
        size_t name_len = 0;
        while (name_len < tag_len && (isalnum((unsigned char)tag[name_len]) ||
                                      tag[name_len] == '/')) {
            name_len++;
        }
        char *tag_name = mi_strndup(tag, name_len);
        if (tag_name) {
            char *name = tag_name;
            while (*name == '/') name++;

            const char *attrs[] = {NULL, NULL, NULL, NULL};
            int is_anchor = 0;
            if (!strcasecmp(name, "a") || !strcasecmp(name, "area")) {
                attrs[0] = "href";
                is_anchor = 1;
            } else if (!strcasecmp(name, "img")) {
                attrs[0] = "src";
                attrs[1] = "data-src";
            } else if (!strcasecmp(name, "source")) {
                attrs[0] = "src";
                attrs[1] = "data-src";
            } else if (!strcasecmp(name, "video") || !strcasecmp(name, "audio")) {
                attrs[0] = "src";
                attrs[1] = "poster";
            } else if (!strcasecmp(name, "link")) {
                attrs[0] = "href";
            } else if (!strcasecmp(name, "iframe") || !strcasecmp(name, "embed")) {
                /* A framed document is part of the page a browser shows. */
                attrs[0] = "src";
            }

            /* The link text is what the destination is called on the page. */
            char *text = is_anchor ? link_text(gt + 1, end, name) : NULL;

            for (size_t i = 0; attrs[i]; i++) {
                char *v = attr_value(tag, tag_len, attrs[i]);
                if (v) {
                    offer(out, base, v, text, -1, filter);
                    free(v);
                }
            }
            free(text);
            free(tag_name);
        }
        p = gt + 1;
    }
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

/* A number from any of the field names a listing might use for a size. */
static int64_t size_from(const mi_json *obj) {
    static const char *KEYS[] = {"size", "bytes", "length", "filesize", "contentLength"};
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        const mi_json *v = mi_json_get(obj, KEYS[i]);
        if (v && v->type == MI_JSON_NUMBER && v->number >= 0) return (int64_t)v->number;
    }
    return -1;
}

/* The URL a JSON entry points at, under any of the names a listing might use. */
static const char *url_from(const mi_json *obj) {
    static const char *KEYS[] = {"url", "src", "file", "href", "link", "download", "location"};
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        const char *v = mi_json_get_string(obj, KEYS[i], NULL);
        if (v && *v) return v;
    }
    return NULL;
}

/* Walk a JSON listing. An array, or an object with an array of entries under a
 * common key, is what every listing looks like in practice. */
static void parse_json(const mi_json *doc, const char *base, mi_browse_filter filter,
                       mi_browse_result *out) {
    if (!doc) return;

    if (doc->type == MI_JSON_ARRAY) {
        for (size_t i = 0; i < doc->count; i++) {
            const mi_json *entry = doc->items[i];
            if (entry && entry->type == MI_JSON_OBJECT) {
                const char *u = url_from(entry);
                /* A listing often names its entries; carry that through. */
                const char *name = mi_json_get_string(entry, "name", NULL);
                if (u) offer(out, base, u, name, size_from(entry), filter);
            }
        }
        return;
    }

    if (doc->type == MI_JSON_OBJECT) {
        static const char *KEYS[] = {"items", "files", "entries", "results", "data", "media"};
        for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
            const mi_json *arr = mi_json_get(doc, KEYS[i]);
            if (arr && arr->type == MI_JSON_ARRAY) {
                parse_json(arr, base, filter, out);
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

/* Whether a body looks like HTML rather than JSON. */
static int looks_like_html(const char *body, size_t len) {
    for (size_t i = 0; i < len && i < 512; i++) {
        if (isspace((unsigned char)body[i])) continue;
        return body[i] == '<';
    }
    return 0;
}

int mi_browse_parse(const char *body, size_t len, const char *base,
                    mi_browse_filter filter, mi_browse_result *out) {
    if (!body || !base || !out) return MI_ERR_ARG;
    result_init(out);

    if (looks_like_html(body, len)) {
        out->title = extract_title(body, len);
        parse_html(body, len, base, filter, out);
        return MI_OK;
    }

    /* JSON, if it parses; anything else yields an empty list rather than a
     * failure, because "nothing here" is a real answer for a page with no media. */
    mi_json *doc = mi_json_parse(body);
    if (doc) {
        parse_json(doc, base, filter, out);
        mi_json_value_free(doc);
    }
    return MI_OK;
}

int mi_browse_url(const char *url, const char *user_agent, mi_browse_filter filter,
                  mi_browse_result *out) {
    mi_browse_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.user_agent = user_agent;
    return mi_browse_url_ext(&cfg, url, filter, out);
}

/*
 * Ask the extractor what media the page is, and offer that as one item.
 *
 * This is the fix for a JavaScript-built page. A watch page's markup references
 * no media at all — its own `<video>` src is set by script after load — so the
 * HTML pass finds only the site's chrome: favicons, a touch icon, a logo. The
 * extractor knows the page's media because it understands the site, so when the
 * markup yields nothing playable it is asked, and its answer is added as a
 * `resolved` item.
 *
 * The item's URL is the *page*, not the stream the extractor reported. A stream
 * URL is signed and expires within hours, so carrying it would produce a result
 * that 404s by the time it is clicked; the download path re-resolves the page
 * instead. `filename` is built from the title, so the card reads as the video
 * rather than as a URL.
 *
 * Only offered when the markup found nothing playable, so a plain gallery is not
 * second-guessed, and only under MI_BROWSE_MEDIA — "everything the page
 * references" is a question about the markup, and the extractor's answer is not
 * part of that.
 */
static void offer_extracted(const mi_browse_config *cfg, const char *url,
                            mi_browse_result *out) {
    mi_extract_config ecfg;
    mi_extract_config_init(&ecfg);
    ecfg.binary = cfg->extractor_path;
    ecfg.gallery_binary = cfg->gallery_extractor_path;
    ecfg.cookies_text = cfg->cookies_text;
    ecfg.proxy = cfg->proxy;
    ecfg.impersonate = cfg->impersonate;

    /* Gallery hosts are gallery-dl's job and their markup already lists the
     * images, so only a video site is asked. */
    if (mi_extract_pick(&ecfg, url) == MI_EXTRACT_GALLERY) return;

    mi_extract_result er;
    if (mi_extract(&ecfg, url, &er) != MI_OK) return;

    /* A video the extractor resolved: a title, and formats that say it is real
     * media rather than a page it merely recognised. */
    int has_media = er.format_count > 0;
    if (!has_media && er.entry_count == 0) {
        mi_extract_result_free(&er);
        return;
    }

    if (out->count >= MAX_ITEMS) {
        mi_extract_result_free(&er);
        return;
    }
    mi_browse_item *grown = mi_realloc(out->items, (out->count + 1) * sizeof *grown);
    if (!grown) {
        mi_extract_result_free(&er);
        return;
    }
    out->items = grown;
    mi_browse_item *it = &out->items[out->count++];

    const char *title = (er.title && *er.title) ? er.title : "media";
    it->url = mi_strdup(url);
    it->title = mi_strdup(title);
    it->mime = mi_strdup("video/mp4");
    it->bytes = -1;
    it->resolved = 1;
    it->thumbnail = er.thumbnail ? mi_strdup(er.thumbnail) : NULL;
    /* A filename the OS can open: the title with a video extension. */
    {
        size_t tl = strlen(title);
        char *name = mi_calloc(tl + 6, 1);
        if (name) {
            memcpy(name, title, tl);
            memcpy(name + tl, ".mp4", 5);
            it->filename = name;
        } else {
            it->filename = mi_strdup("media.mp4");
        }
    }

    mi_extract_result_free(&er);
}

int mi_browse_url_ext(const mi_browse_config *cfg, const char *url,
                      mi_browse_filter filter, mi_browse_result *out) {
    if (!url || !out) return MI_ERR_ARG;
    result_init(out);

    mi_download_config dcfg;
    mi_download_config_init(&dcfg);
    /* A listing page is not itself media, so the media check must not apply. */
    dcfg.require_media = 0;
    /* Some hosts refuse a request that does not look like a browser, so the
     * operator can say what Vyzor should call itself. */
    dcfg.user_agent = cfg ? cfg->user_agent : NULL;
    if (cfg) {
        dcfg.cookies_text = cfg->cookies_text;
        dcfg.proxy = cfg->proxy;
        dcfg.impersonate = cfg->impersonate;
    }

    char *body = mi_download_fetch_text(&dcfg, url, MAX_BODY);
    if (!body) return MI_ERR_NET;

    /* Extract against the URL as given: redirects are followed by the fetcher,
     * and a relative link on the page is relative to what the browser would
     * have shown, which is the final URL. */
    int rc = mi_browse_parse(body, strlen(body), url, filter, out);
    free(body);
    if (rc != MI_OK) return rc;

    /*
     * A page whose markup yielded no playable media, under MEDIA mode, is asked
     * of the extractor: the only way a JavaScript-built page (a watch page)
     * yields its media at all. A page that already listed media is left as it is.
     */
    if (cfg && cfg->use_extractor && filter == MI_BROWSE_MEDIA) {
        int playable = 0;
        for (size_t i = 0; i < out->count; i++) {
            const char *m = out->items[i].mime;
            if (m && (!strncmp(m, "video/", 6) || !strncmp(m, "audio/", 6) ||
                      !strcmp(m, "application/vnd.apple.mpegurl") ||
                      !strcmp(m, "application/dash+xml"))) {
                playable = 1;
                break;
            }
        }
        if (!playable) offer_extracted(cfg, url, out);
    }
    return MI_OK;
}
