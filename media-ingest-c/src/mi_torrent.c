/* mi_torrent.c - bencode, and what a .torrent file says.
 *
 * The parser is a small recursive-descent reader over a byte buffer. It is
 * defensive on purpose: a .torrent is untrusted input from the internet, so a
 * length past the end, a runaway nesting depth, or a missing dictionary is a
 * clean rejection rather than a read out of bounds.
 */
#define _GNU_SOURCE
#include "mi_torrent.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mi_util.h"

/* A torrent nests a few levels (root -> info -> files -> file -> path); 32 is
 * far past anything real and stops a crafted `llllll...` from exhausting the
 * stack. */
#define BEN_MAX_DEPTH 32

typedef enum { BEN_INT, BEN_STR, BEN_LIST, BEN_DICT } ben_type;

typedef struct ben_value {
    ben_type type;
    /* integers */
    int64_t i;
    /* strings: a view into the input, never owned */
    const unsigned char *s;
    size_t slen;
    /* lists and dicts */
    struct ben_value **items;
    /* dict keys, parallel to `items` */
    const unsigned char **keys;
    size_t *keylens;
    size_t count;
    /* the byte span this value occupied, so the `info` dict can be re-hashed
     * exactly as it appeared */
    size_t start;
    size_t end;
} ben_value;

typedef struct {
    const unsigned char *p;
    size_t len;
    size_t pos;
    int depth;
} ben_ctx;

static void ben_free(ben_value *v) {
    if (!v) return;
    for (size_t i = 0; i < v->count; i++) ben_free(v->items[i]);
    free(v->items);
    free(v->keys);
    free(v->keylens);
    free(v);
}

static ben_value *ben_new(ben_type t) {
    ben_value *v = mi_calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

/* A decimal integer, bounds-checked against overflow. */
static int ben_int(ben_ctx *c, int64_t *out) {
    if (c->pos >= c->len || c->p[c->pos] != 'i') return MI_ERR_ARG;
    c->pos++;
    int neg = 0;
    if (c->pos < c->len && c->p[c->pos] == '-') {
        neg = 1;
        c->pos++;
    }
    if (c->pos >= c->len || !isdigit(c->p[c->pos])) return MI_ERR_ARG;
    int64_t v = 0;
    while (c->pos < c->len && isdigit(c->p[c->pos])) {
        int d = c->p[c->pos] - '0';
        if (v > (INT64_MAX - d) / 10) return MI_ERR_ARG;
        v = v * 10 + d;
        c->pos++;
    }
    if (c->pos >= c->len || c->p[c->pos] != 'e') return MI_ERR_ARG;
    c->pos++;
    *out = neg ? -v : v;
    return MI_OK;
}

/* `<len>:<bytes>` — the bytes are a view into the input. */
static int ben_str(ben_ctx *c, const unsigned char **s, size_t *n) {
    if (c->pos >= c->len || !isdigit(c->p[c->pos])) return MI_ERR_ARG;
    size_t v = 0;
    while (c->pos < c->len && isdigit(c->p[c->pos])) {
        int d = c->p[c->pos] - '0';
        if (v > (SIZE_MAX - (size_t)d) / 10) return MI_ERR_ARG;
        v = v * 10 + (size_t)d;
        c->pos++;
    }
    if (c->pos >= c->len || c->p[c->pos] != ':') return MI_ERR_ARG;
    c->pos++;
    /* The length must fit inside what is left, or a crafted length reads past
     * the buffer. */
    if (v > c->len - c->pos) return MI_ERR_ARG;
    *s = c->p + c->pos;
    *n = v;
    c->pos += v;
    return MI_OK;
}

static ben_value *ben_parse(ben_ctx *c);

static ben_value *ben_parse_list(ben_ctx *c) {
    if (c->depth++ > BEN_MAX_DEPTH) {
        c->depth--;
        return NULL;
    }
    c->pos++; /* 'l' */
    ben_value *v = ben_new(BEN_LIST);
    if (!v) {
        c->depth--;
        return NULL;
    }
    while (c->pos < c->len && c->p[c->pos] != 'e') {
        ben_value *item = ben_parse(c);
        if (!item) {
            ben_free(v);
            c->depth--;
            return NULL;
        }
        ben_value **grown = mi_realloc(v->items, (v->count + 1) * sizeof *grown);
        if (!grown) {
            ben_free(item);
            ben_free(v);
            c->depth--;
            return NULL;
        }
        v->items = grown;
        v->items[v->count++] = item;
    }
    if (c->pos >= c->len) {
        ben_free(v);
        c->depth--;
        return NULL;
    }
    c->pos++; /* 'e' */
    c->depth--;
    return v;
}

static ben_value *ben_parse_dict(ben_ctx *c) {
    if (c->depth++ > BEN_MAX_DEPTH) {
        c->depth--;
        return NULL;
    }
    c->pos++; /* 'd' */
    ben_value *v = ben_new(BEN_DICT);
    if (!v) {
        c->depth--;
        return NULL;
    }
    while (c->pos < c->len && c->p[c->pos] != 'e') {
        const unsigned char *k = NULL;
        size_t klen = 0;
        if (ben_str(c, &k, &klen) != MI_OK) {
            ben_free(v);
            c->depth--;
            return NULL;
        }
        ben_value *val = ben_parse(c);
        if (!val) {
            ben_free(v);
            c->depth--;
            return NULL;
        }
        ben_value **grown = mi_realloc(v->items, (v->count + 1) * sizeof *grown);
        const unsigned char **kgrown =
            mi_realloc(v->keys, (v->count + 1) * sizeof *kgrown);
        size_t *lgrown = mi_realloc(v->keylens, (v->count + 1) * sizeof *lgrown);
        if (!grown || !kgrown || !lgrown) {
            free(grown);
            free(kgrown);
            free(lgrown);
            ben_free(val);
            ben_free(v);
            c->depth--;
            return NULL;
        }
        v->items = grown;
        v->keys = kgrown;
        v->keylens = lgrown;
        v->keys[v->count] = k;
        v->keylens[v->count] = klen;
        v->items[v->count] = val;
        v->count++;
    }
    if (c->pos >= c->len) {
        ben_free(v);
        c->depth--;
        return NULL;
    }
    c->pos++; /* 'e' */
    c->depth--;
    return v;
}

static ben_value *ben_parse(ben_ctx *c) {
    if (c->pos >= c->len) return NULL;
    size_t start = c->pos;
    ben_value *v = NULL;
    unsigned char ch = c->p[c->pos];
    if (ch == 'i') {
        int64_t n = 0;
        if (ben_int(c, &n) != MI_OK) return NULL;
        v = ben_new(BEN_INT);
        if (v) v->i = n;
    } else if (ch == 'l') {
        v = ben_parse_list(c);
    } else if (ch == 'd') {
        v = ben_parse_dict(c);
    } else if (isdigit(ch)) {
        const unsigned char *s = NULL;
        size_t n = 0;
        if (ben_str(c, &s, &n) != MI_OK) return NULL;
        v = ben_new(BEN_STR);
        if (v) {
            v->s = s;
            v->slen = n;
        }
    }
    if (v) {
        v->start = start;
        v->end = c->pos;
    }
    return v;
}

/* Look up a byte-string key in a dict. */
static const ben_value *ben_get(const ben_value *d, const char *key) {
    if (!d || d->type != BEN_DICT) return NULL;
    size_t klen = strlen(key);
    for (size_t i = 0; i < d->count; i++) {
        if (d->keylens[i] == klen && memcmp(d->keys[i], key, klen) == 0) return d->items[i];
    }
    return NULL;
}

/* A byte string as an owned C string, or NULL. */
static char *ben_dup_str(const ben_value *v) {
    if (!v || v->type != BEN_STR) return NULL;
    char *out = mi_malloc(v->slen + 1);
    if (!out) return NULL;
    memcpy(out, v->s, v->slen);
    out[v->slen] = '\0';
    return out;
}

static int64_t ben_int_or(const ben_value *v, int64_t dflt) {
    return (v && v->type == BEN_INT) ? v->i : dflt;
}

static void hex20(const unsigned char *in, char *out /* 41 */) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[i * 2] = HEX[in[i] >> 4];
        out[i * 2 + 1] = HEX[in[i] & 0xf];
    }
    out[40] = '\0';
}

/* ---- base32 (RFC 4648, no padding), for the magnet hash's other spelling ---- */

static int base32_decode(const char *in, size_t inlen, unsigned char *out, size_t cap,
                         size_t *outlen) {
    int bits = 0;
    unsigned int acc = 0;
    size_t n = 0;
    for (size_t i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)toupper((unsigned char)in[i]);
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else return MI_ERR_ARG;
        acc = (acc << 5) | (unsigned int)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return MI_ERR_ARG;
            out[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    *outlen = n;
    return MI_OK;
}

/* ---- URL escaping, for building a magnet ---- */

static int is_unreserved(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static char *url_encode(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = mi_malloc(n * 3 + 1);
    if (!out) return NULL;
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_unreserved(c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 0xf];
        }
    }
    out[o] = '\0';
    return out;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *url_decode(const char *s, size_t len) {
    char *out = mi_malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len) {
            int hi = hexval((unsigned char)s[i + 1]);
            int lo = hexval((unsigned char)s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        /* A magnet's display name uses '+' for a space, as a query string does. */
        out[o++] = s[i] == '+' ? ' ' : s[i];
    }
    out[o] = '\0';
    return out;
}

/* ---- the public surface ---- */

int mi_torrent_is_magnet(const char *url) {
    return url && strncasecmp(url, "magnet:?", 8) == 0;
}

int mi_torrent_hash_from_magnet(const char *magnet, char *out_hex) {
    if (!magnet || !out_hex) return MI_ERR_ARG;
    const char *p = strcasestr(magnet, "xt=urn:btih:");
    if (!p) return MI_ERR_ARG;
    p += strlen("xt=urn:btih:");
    const char *end = p;
    while (*end && *end != '&' && *end != '#') end++;
    size_t n = (size_t)(end - p);
    if (n == 40) {
        for (size_t i = 0; i < 40; i++) {
            if (hexval((unsigned char)p[i]) < 0) return MI_ERR_ARG;
        }
        for (size_t i = 0; i < 40; i++) out_hex[i] = (char)tolower((unsigned char)p[i]);
        out_hex[40] = '\0';
        return MI_OK;
    }
    if (n == 32) {
        unsigned char raw[20];
        size_t rawlen = 0;
        if (base32_decode(p, n, raw, sizeof raw, &rawlen) != MI_OK) return MI_ERR_ARG;
        if (rawlen != 20) return MI_ERR_ARG;
        hex20(raw, out_hex);
        return MI_OK;
    }
    return MI_ERR_ARG;
}

char *mi_torrent_name_from_magnet(const char *magnet) {
    if (!magnet) return NULL;
    const char *p = strcasestr(magnet, "dn=");
    if (!p) return NULL;
    p += 3;
    const char *end = p;
    while (*end && *end != '&' && *end != '#') end++;
    if (end == p) return NULL;
    return url_decode(p, (size_t)(end - p));
}

char *mi_torrent_build_magnet(const char *info_hash_hex, const char *name,
                              const char *const *trackers, size_t tracker_count) {
    if (!info_hash_hex || !*info_hash_hex) return NULL;

    size_t cap = 64 + strlen(info_hash_hex);
    if (name && *name) cap += 8 + strlen(name) * 3;
    for (size_t i = 0; i < tracker_count; i++) {
        if (trackers[i]) cap += 12 + strlen(trackers[i]) * 3;
    }
    char *out = mi_malloc(cap);
    if (!out) return NULL;

    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "magnet:?xt=urn:btih:%s", info_hash_hex);
    if (name && *name) {
        char *enc = url_encode(name);
        if (enc) {
            o += (size_t)snprintf(out + o, cap - o, "&dn=%s", enc);
            free(enc);
        }
    }
    for (size_t i = 0; i < tracker_count; i++) {
        if (!trackers[i] || !*trackers[i]) continue;
        char *enc = url_encode(trackers[i]);
        if (enc) {
            o += (size_t)snprintf(out + o, cap - o, "&tr=%s", enc);
            free(enc);
        }
    }
    return out;
}

int mi_torrent_parse(const unsigned char *data, size_t len, mi_torrent *out) {
    if (!data || !out) return MI_ERR_ARG;
    memset(out, 0, sizeof *out);

    ben_ctx c = {.p = data, .len = len, .pos = 0, .depth = 0};
    ben_value *root = ben_parse(&c);
    if (!root) return MI_ERR_ARG;
    if (root->type != BEN_DICT) {
        ben_free(root);
        return MI_ERR_ARG;
    }
    const ben_value *info = ben_get(root, "info");
    if (!info || info->type != BEN_DICT) {
        ben_free(root);
        return MI_ERR_ARG;
    }

    /* The info-hash is the SHA-1 of the info dictionary *as it appeared*, which
     * is why the parser records the span rather than re-serialising. */
    unsigned char digest[20];
    mi_sha1(data + info->start, info->end - info->start, digest);
    out->info_hash = mi_malloc(41);
    if (!out->info_hash) {
        ben_free(root);
        return MI_ERR_NOMEM;
    }
    hex20(digest, out->info_hash);

    out->name = ben_dup_str(ben_get(info, "name"));
    out->piece_length = ben_int_or(ben_get(info, "piece length"), 0);
    const ben_value *pieces = ben_get(info, "pieces");
    out->piece_count = (pieces && pieces->type == BEN_STR) ? (int)(pieces->slen / 20) : 0;
    out->is_private = ben_int_or(ben_get(info, "private"), 0) != 0;

    const ben_value *files = ben_get(info, "files");
    if (files && files->type == BEN_LIST && files->count > 0) {
        /* Multi-file: each entry is {length, path: [segments]}. */
        out->files = mi_calloc(files->count, sizeof *out->files);
        if (!out->files) {
            ben_free(root);
            mi_torrent_free(out);
            return MI_ERR_NOMEM;
        }
        for (size_t i = 0; i < files->count; i++) {
            const ben_value *f = files->items[i];
            if (!f || f->type != BEN_DICT) continue;
            int64_t flen = ben_int_or(ben_get(f, "length"), 0);
            const ben_value *path = ben_get(f, "path");
            /* The full relative path starts with the torrent's name, which is
             * where a client writes it. */
            size_t need = out->name ? strlen(out->name) : 0;
            size_t segs = (path && path->type == BEN_LIST) ? path->count : 0;
            for (size_t k = 0; k < segs; k++) {
                const ben_value *seg = path->items[k];
                if (seg && seg->type == BEN_STR) need += 1 + seg->slen;
            }
            char *full = mi_malloc(need + 1);
            if (!full) {
                ben_free(root);
                mi_torrent_free(out);
                return MI_ERR_NOMEM;
            }
            size_t o = 0;
            if (out->name) {
                memcpy(full, out->name, strlen(out->name));
                o = strlen(out->name);
            }
            for (size_t k = 0; k < segs; k++) {
                const ben_value *seg = path->items[k];
                if (!seg || seg->type != BEN_STR) continue;
                if (o > 0) full[o++] = '/';
                memcpy(full + o, seg->s, seg->slen);
                o += seg->slen;
            }
            full[o] = '\0';
            out->files[out->file_count].path = full;
            out->files[out->file_count].bytes = flen;
            out->file_count++;
            out->total_bytes += flen;
        }
    } else {
        /* Single-file: the name *is* the file. */
        int64_t flen = ben_int_or(ben_get(info, "length"), 0);
        out->files = mi_calloc(1, sizeof *out->files);
        if (!out->files) {
            ben_free(root);
            mi_torrent_free(out);
            return MI_ERR_NOMEM;
        }
        out->files[0].path = out->name ? mi_strdup(out->name) : mi_strdup("download");
        out->files[0].bytes = flen;
        out->file_count = 1;
        out->total_bytes = flen;
    }

    /* Trackers: `announce`, plus every tier of `announce-list`. */
    size_t cap = 0;
    const ben_value *ann = ben_get(root, "announce");
    const ben_value *annl = ben_get(root, "announce-list");
    size_t total = (ann && ann->type == BEN_STR ? 1 : 0);
    if (annl && annl->type == BEN_LIST) {
        for (size_t i = 0; i < annl->count; i++) {
            const ben_value *tier = annl->items[i];
            if (tier && tier->type == BEN_LIST) total += tier->count;
        }
    }
    if (total > 0) {
        out->trackers = mi_calloc(total, sizeof *out->trackers);
        if (out->trackers) {
            cap = total;
            /* `announce` first, then the tiers. A duplicate is skipped so a file
             * that lists the same tracker twice does not hand aria2 two. */
            if (ann && ann->type == BEN_STR) {
                char *s = ben_dup_str(ann);
                if (s) out->trackers[out->tracker_count++] = s;
            }
            if (annl && annl->type == BEN_LIST) {
                for (size_t i = 0; i < annl->count; i++) {
                    const ben_value *tier = annl->items[i];
                    if (!tier || tier->type != BEN_LIST) continue;
                    for (size_t k = 0; k < tier->count; k++) {
                        char *s = ben_dup_str(tier->items[k]);
                        if (!s) continue;
                        int dup = 0;
                        for (size_t t = 0; t < out->tracker_count; t++) {
                            if (strcmp(out->trackers[t], s) == 0) {
                                dup = 1;
                                break;
                            }
                        }
                        if (dup || out->tracker_count >= cap) {
                            free(s);
                            continue;
                        }
                        out->trackers[out->tracker_count++] = s;
                    }
                }
            }
        }
    }

    out->comment = ben_dup_str(ben_get(root, "comment"));
    out->created_by = ben_dup_str(ben_get(root, "created by"));
    out->created_at = ben_int_or(ben_get(root, "creation date"), 0);
    out->magnet = mi_torrent_build_magnet(out->info_hash, out->name,
                                          (const char *const *)out->trackers,
                                          out->tracker_count);

    ben_free(root);
    return MI_OK;
}

void mi_torrent_free(mi_torrent *t) {
    if (!t) return;
    for (size_t i = 0; i < t->file_count; i++) free(t->files[i].path);
    free(t->files);
    for (size_t i = 0; i < t->tracker_count; i++) free(t->trackers[i]);
    free(t->trackers);
    free(t->name);
    free(t->info_hash);
    free(t->comment);
    free(t->created_by);
    free(t->magnet);
    memset(t, 0, sizeof *t);
}
