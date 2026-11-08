/* mi_torrent.h - read a .torrent file, and understand a magnet link.
 *
 * WHY THIS EXISTS
 *
 * A torrent index (1337x) hands back a magnet link, not a file. A magnet is only
 * an info-hash and a name: everything else about the torrent — its files, their
 * sizes, the tracker list — is in the *metadata*, which a BitTorrent client
 * fetches from the swarm. That is fine for a download (aria2 does it) but it
 * means the console cannot show what a torrent *contains* until something has
 * looked it up.
 *
 * A `.torrent` file is the opposite: it *is* the metadata, bencoded. So this
 * module reads one, and the two paths meet at the same place — the info-hash,
 * which is what a magnet names.
 *
 * WHAT THIS IS NOT
 *
 * It does not talk to a swarm. Fetching metadata for a bare magnet needs a
 * BitTorrent peer exchange, which is the client's job; here a magnet yields its
 * hash and its display name, and nothing more until it is downloaded.
 *
 * The parser is deliberately strict about bounds: a bencode file is untrusted
 * input from the internet, and a length that runs past the buffer must be a
 * clean rejection rather than a read.
 */
#ifndef MI_TORRENT_H
#define MI_TORRENT_H

#include <stddef.h>
#include <stdint.h>

/* One file inside a torrent. */
typedef struct {
    /* The path relative to the torrent's root, joined with '/'. */
    char *path;
    int64_t bytes;
} mi_torrent_file;

typedef struct {
    /* The torrent's name (the `info.name`), and the display name. */
    char *name;
    /* The info-hash, 40 lowercase hex. This is what a magnet names. */
    char *info_hash;
    /* The sum of every file's length. */
    int64_t total_bytes;
    int64_t piece_length;
    int piece_count;
    mi_torrent_file *files;
    size_t file_count;
    /* `announce` plus every tier of `announce-list`, de-duplicated. */
    char **trackers;
    size_t tracker_count;
    /*
     * BEP-19 webseeds (`url-list`), as base URLs.
     *
     * An HTTP source for the content, which is how an archive.org torrent is
     * fetched at all — it has no peers. Also what makes a *poster* possible: an
     * image file inside the torrent can be pointed at `<webseed>/<path>` and
     * loaded directly, without downloading the torrent.
     */
    char **webseeds;
    size_t webseed_count;
    char *comment;
    char *created_by;
    /* Unix epoch seconds, 0 when the file did not say. */
    int64_t created_at;
    /* 1 when the torrent is marked private (no DHT/PEX). */
    int is_private;
    /*
     * A magnet URI built from the info-hash, the name and the trackers.
     *
     * Built here so a `.torrent` the user opened can be handed to the download
     * path as a magnet, which is the one shape aria2 and the UI both take.
     */
    char *magnet;
} mi_torrent;

/*
 * Parse bencoded `.torrent` bytes.
 *
 * Returns MI_OK, or a negative MI_ERR_* when the bytes are not a torrent: a
 * missing `info` dictionary, a malformed bencode, or a length that runs past the
 * end. `out` is zeroed first, so a failure leaves nothing to free.
 */
int mi_torrent_parse(const unsigned char *data, size_t len, mi_torrent *out);

void mi_torrent_free(mi_torrent *t);

/*
 * The info-hash out of a magnet URI, as 40 lowercase hex.
 *
 * Accepts the 40-hex form and the 32-character base32 form, because both appear
 * in the wild. Returns MI_OK, or MI_ERR_ARG when there is no usable `xt`.
 */
int mi_torrent_hash_from_magnet(const char *magnet, char *out_hex /* >= 41 */);

/*
 * The display name out of a magnet URI (`dn=`), URL-decoded and '+' expanded.
 * Returns an owned string, or NULL when the magnet carries no name.
 */
char *mi_torrent_name_from_magnet(const char *magnet);

/*
 * Build a magnet URI from an info-hash, a name and optional trackers.
 * Returns an owned string.
 */
char *mi_torrent_build_magnet(const char *info_hash_hex, const char *name,
                              const char *const *trackers, size_t tracker_count);

/** Does this URL look like a magnet link? */
int mi_torrent_is_magnet(const char *url);

#endif /* MI_TORRENT_H */
