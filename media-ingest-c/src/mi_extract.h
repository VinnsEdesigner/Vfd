/* mi_extract.h - the URLs a streaming site will not put in its HTML.
 *
 * WHY THIS IS A SEPARATE MODULE
 *
 * A crawl reads a page's links. That works for a datasheet host and fails
 * completely for a streaming site: the player's media URL is not in the HTML,
 * it is signed, and the signature is computed by obfuscated JavaScript at play
 * time. Measured on one YouTube watch page: the HTML carries `signatureCipher`
 * entries and no usable `url`, so a crawl finds nothing and the traffic watcher
 * sees only thumbnails.
 *
 * yt-dlp exists because that problem is large and changes constantly. It is the
 * maintained implementation of the transforms these sites require, and it
 * returns a plain list of URLs with their formats — which is the only thing
 * Vyzor actually needs.
 *
 * WHAT THIS IS NOT
 *
 * It is not a downloader. yt-dlp is asked to *resolve* a page, not to fetch the
 * media: the URLs it prints are handed to Vyzor's own download path, which then
 * reads them (through ffmpeg, for a manifest). One tool for finding the stream,
 * one for fetching it.
 *
 * And it is a fallback, not the first move. A crawl is tried first, because most
 * sites do not need this and shelling out to a Python program is not free.
 */
#ifndef MI_EXTRACT_H
#define MI_EXTRACT_H

#include <stddef.h>
#include <stdint.h>

/* One way of fetching the media, as the extractor describes it. */
typedef struct {
    char *url;
    /* The extractor's own id for the format, e.g. "137" or "22". */
    char *format_id;
    char *ext;
    /* "1920x1080" or NULL when the format does not say. */
    char *resolution;
    /* Video and audio codec names, or "none" for a muxed-away stream. */
    char *vcodec;
    char *acodec;
    /* The media type, e.g. "video/mp4". */
    char *mime;
    int64_t filesize;      /* -1 when unknown */
    double bitrate;        /* -1 when unknown */
    int height;            /* 0 when unknown */
    /* 1 when this format carries both video and audio, so it can be saved
     * as-is. A video-only or audio-only format needs the two muxed. */
    int has_video;
    int has_audio;
    /*
     * The extractor's transfer protocol, e.g. "https" or "m3u8_native".
     *
     * It decides whether the URL can be fetched at all. A direct protocol is a
     * file a downloader can range; "m3u8_native" is an HLS *variant* — a
     * playlist, not media — and handing it to a plain fetcher fetches the
     * playlist. NULL when the extractor did not say.
     */
    char *protocol;
    /*
     * 1 when this format is a direct fetch (http/https), 0 when it is a
     * manifest a reader must follow (m3u8, m3u8_native, dash, mhtml).
     *
     * Used to prefer a format that can actually be fetched: YouTube lists its
     * highest renditions as HLS variants, and choosing one by height alone
     * picked a URL no plain downloader can use.
     */
    int direct;
} mi_extract_format;

/* One item in a playlist, channel or search result. */
typedef struct {
    char *id;
    char *title;
    /* The page URL a second call would resolve. */
    char *url;
    char *uploader;
    double duration_seconds; /* -1 when unknown */
    char *thumbnail;
    /* 1 when this entry is itself a playlist rather than a video. */
    int is_playlist;
} mi_extract_entry;

typedef struct {
    /* The page's own title, when the extractor reported one. */
    char *title;
    /* The site it came from, e.g. "youtube". */
    char *extractor;
    double duration_seconds; /* -1 when unknown */
    /* A still image for the media, when there is one. */
    char *thumbnail;
    mi_extract_format *formats;
    size_t format_count;
    /*
     * The entries of a playlist, channel or search.
     *
     * A list URL describes many items and `--flat-playlist` returns them without
     * resolving each one, so these carry a title, an id and a page URL but no
     * stream URLs — a chosen one is resolved in a second call. Empty for a
     * single item, which is the common case.
     */
    mi_extract_entry *entries;
    size_t entry_count;
} mi_extract_result;

void mi_extract_result_free(mi_extract_result *res);

/*
 * Which extractor to ask.
 *
 * Three tools, complementary rather than competing:
 *
 *   yt-dlp      video. Streams, formats, and it knows how to pick a rendition.
 *   gallery-dl  images, galleries and posts. 4,927 sites against yt-dlp's
 *               1,755, and most of the extra are image hosts.
 *   py1337x     torrents. A BitTorrent index carries no media URL and no file
 *               listing in its HTML: the download is a magnet link, and the
 *               files behind it are only known to a BitTorrent client. Neither
 *               yt-dlp nor gallery-dl resolves one, so a torrent index is its
 *               own kind.
 *
 * A photo site is gallery-dl's job, a video site is yt-dlp's and a torrent index
 * is py1337x's, so the choice is made per URL. `AUTO` prefers gallery-dl for a
 * host it recognises and the torrent extractor for a host that indexes torrents,
 * because asking yt-dlp about either usually returns nothing at all.
 *
 * The host lists are deliberately short and explicit: guessing from a URL's shape
 * would send a video to the wrong tool, and a wrong guess costs the user an empty
 * result they cannot explain.
 */
typedef enum {
    MI_EXTRACT_AUTO = 0, /* pick by host */
    MI_EXTRACT_VIDEO,    /* yt-dlp */
    MI_EXTRACT_GALLERY,  /* gallery-dl */
    MI_EXTRACT_TORRENT   /* py1337x, for a torrent index */
} mi_extract_kind;

typedef struct {
    /*
     * The extractor binary. NULL uses "yt-dlp" from PATH.
     *
     * Named rather than hardcoded so an operator can point at their own build;
     * a distribution's copy can lag a site's changes by months.
     */
    const char *binary;
    /*
     * The gallery extractor's binary. NULL uses "gallery-dl" from PATH.
     *
     * A second tool rather than a second mode: it answers a different question —
     * "what images are on this page" rather than "what streams does this video
     * have" — and returns a different shape, so it has its own parser.
     */
    const char *gallery_binary;
    /*
     * The torrent extractor's binary. NULL uses "py1337x" from PATH.
     *
     * py1337x is a Python library, not a CLI, so this is a wrapper script that
     * prints one yt-dlp-shaped JSON object per call — see `scripts/py1337x-json.py`.
     * Named rather than hardcoded so an operator can point at their own copy, the
     * same reason the other two are.
     */
    const char *torrent_binary;
    /*
     * A BitTorrent index to search instead of a URL, e.g. "1337x".
     *
     * A torrent index is not a single page you resolve; it is a search box. When
     * this is set, the torrent extractor searches it for `search_term` and returns
     * one result per torrent. The name is the extractor's own key, so the wrapper
     * decides which index a name means.
     *
     * NULL searches the extractor's default index.
     */
    const char *torrent_site;
    /* Which extractor to use. AUTO picks by host. */
    mi_extract_kind kind;
    /* Seconds allowed for the resolve. It is a network operation against a
     * site that may be slow to answer, so this is generous. */
    double timeout_seconds;
    /*
     * Whether to enumerate a playlist or channel rather than resolve one item.
     *
     * `--flat-playlist` lists the entries without resolving each one, which is
     * far cheaper and is what a discovery pass needs. Off by default: a caller
     * usually has one video in mind.
     */
    int enumerate;
    /* Cookies, in Netscape format, for media that is not public — a private or
     * unlisted video the caller has rights to. NULL sends none. */
    const char *cookies_file;
    /*
     * The same cookies, as an inline string, for a caller that has them in hand
     * rather than on disk.
     *
     * Netscape format, same as the file. A server holding a credential for a
     * user has it in memory and should not have to write it to disk to pass it
     * on — a file is one more place a secret can outlive its use.
     */
    const char *cookies_text;
    /*
     * Load cookies straight from an installed browser's profile instead of a
     * file, as yt-dlp's `--cookies-from-browser` (e.g. "chrome", "firefox",
     * "chromium"). NULL or "" is off.
     *
     * This is the alternative to an export the operator has to make by hand: on
     * a machine where the operator is already signed in to the site in a
     * browser, yt-dlp reads that session directly. It is deliberately *not* a
     * fallback — yt-dlp aborts the whole run when the named browser has no
     * cookie store, rather than continuing anonymously — so it is used only when
     * a caller asks for it by name.
     *
     * Takes precedence over the file and text above, because yt-dlp gives it
     * precedence: naming both would make the other silently ineffective.
     */
    const char *cookies_from_browser;
    /*
     * A proxy the extractor should send its requests through, e.g.
     * "http://host:port" or "socks5://host:port".
     *
     * A site may refuse a request from the host's own address — a datacenter
     * range, or a country — and a proxy is the way out. yt-dlp takes this as
     * `--proxy`, so it is passed through unchanged. NULL sends direct.
     */
    const char *proxy;
    /*
     * A yt-dlp / curl_cffi impersonation target, e.g. "chrome" or "chrome131".
     *
     * A site that fingerprints the TLS handshake sees libcurl and refuses it;
     * yt-dlp can wear a real browser's fingerprint when told to. NULL is off.
     */
    const char *impersonate;
    /*
     * A search to run instead of resolving a URL.
     *
     * yt-dlp answers `ytsearch<N>:term` with a list of matching videos, so a
     * caller who knows *what* they want but not *where* it is can find it. When
     * this is set, `url` is ignored and the query is run instead — the two are
     * different questions and one call does one of them.
     *
     * A search is always enumerated (there is no single result to resolve) and
     * the entries carry titles and ids but no stream URLs, so a chosen entry is
     * resolved in a second call.
     */
    const char *search_term;
    /* The most search results to return. 0 uses a small default. */
    int search_limit;
    /*
     * Only items from this date onward, as YYYYMMDD.
     *
     * A crawler's usual next question after a name: "what is new?" The
     * extractor applies it while listing, so an old item is never resolved.
     * NULL means no bound.
     */
    const char *date_after;
    /* Only items up to this date, as YYYYMMDD. NULL means no bound. */
    const char *date_before;
    /*
     * Which entries of a list to take, in the extractor's own syntax: "1:5" for
     * a range, "1,3,5" for a set, "-5:" for the last five.
     *
     * This is how a channel crawl is bounded. Without it, listing a channel with
     * thousands of videos returns thousands of entries — the caller usually
     * wants the newest few, and saying so costs nothing where filtering
     * afterwards would mean transferring them all.
     */
    const char *playlist_items;
    /*
     * A generic filter the extractor applies while listing, in its own syntax:
     * `duration > 600`, `view_count > 1000`, `title ~= "tutorial"`.
     *
     * Applied by the extractor rather than by Vyzor because it can be evaluated
     * from the list metadata, so a filtered-out item is never resolved.
     */
    const char *match_filter;
    /*
     * Per-site arguments, `IE_KEY:ARGS` — the escape hatch for a site that needs
     * a nudge, e.g. `youtube:player_client=web`.
     *
     * Worth having because the alternative when a site changes is waiting for an
     * extractor release. Passed through verbatim; it is the extractor's syntax,
     * not ours, and inventing a wrapper would only get in the way.
     */
    const char *const *extractor_args;
    size_t extractor_arg_count;
} mi_extract_config;

void mi_extract_config_init(mi_extract_config *cfg);

/*
 * Netscape cookie text as ffmpeg's `-cookies` string, or NULL.
 *
 * The two tools want the same credential in different shapes: the extractor takes
 * a Netscape file, ffmpeg takes newline-delimited `Set-Cookie` values. This is the
 * conversion, exposed because the download path needs it — ffmpeg does the
 * fetching, so it is ffmpeg that must send the cookies.
 *
 * Returns an owned string the caller frees, or NULL when there is nothing usable.
 */
char *mi_extract_cookies_for_ffmpeg(const char *text);

/*
 * An impersonation target in the form yt-dlp and curl_cffi accept.
 *
 * Both reject a display name or a separator — curl_cffi wants `chrome131` and
 * refuses `Chrome131` or `chrome-131`, which is what a settings box collects —
 * so a target is lowercased with `-`, `_` and spaces removed. A bare family
 * ("chrome") is returned unchanged.
 *
 * Returns an owned string the caller frees, or NULL when there is nothing.
 */
char *mi_extract_normalize_impersonate(const char *target);

/*
 * Write Netscape cookie text to a 0600 temp file and return its path.
 *
 * yt-dlp takes cookies as a path, so a caller holding them inline (a server
 * with a user's credential in memory) needs one. The file is the caller's to
 * unlink; it holds a credential, so it should be removed the moment the child
 * that needed it has exited.
 *
 * Returns 0 on success, non-zero on failure (out_path untouched).
 */
int mi_extract_write_cookies_file(const char *text, char **out_path);

/*
 * The extractor to use for a URL, honouring an explicit choice.
 *
 * Exposed so a caller can say which it picked, and so the choice can be tested
 * without running a subprocess.
 */
mi_extract_kind mi_extract_pick(const mi_extract_config *cfg, const char *url);

/*
 * Whether the extractor of that kind is usable: the binary exists and answers.
 *
 * Called before offering the feature, so the console never shows a control that
 * cannot work. Cached after the first call — it does not change at runtime.
 */
int mi_extract_available_kind(const mi_extract_config *cfg, mi_extract_kind kind);

/*
 * The name of a tool, for a log line: "yt-dlp", "gallery-dl" or "py1337x".
 *
 * Exposed so a caller can name the tool it asked without repeating the mapping,
 * and so the mapping can be tested.
 */
const char *mi_extract_kind_name(mi_extract_kind kind);

/*
 * Ask the extractor what `url` offers.
 *
 * Returns MI_OK when the page was resolved, even if it yielded no formats —
 * "resolved and there is nothing here" is a result. A negative MI_ERR_* means
 * the extractor could not run or refused the URL.
 */
int mi_extract(const mi_extract_config *cfg, const char *url, mi_extract_result *out);

/*
 * Whether the extractor is usable: the binary exists and answers.
 *
 * Called before offering the feature, so the console never shows a control that
 * cannot work. Cached after the first call — it does not change at runtime.
 */
int mi_extract_available(const mi_extract_config *cfg);

#endif /* MI_EXTRACT_H */
