#!/usr/bin/env python3
"""py1337x as a JSON CLI, for Vyzor's extractor seam.

Vyzor asks an external tool to resolve a page and reads one yt-dlp-shaped JSON
object from its stdout (see `media-ingest-c/src/mi_extract.c`). yt-dlp and
gallery-dl are already CLIs; py1337x is a *library*, so this wrapper is the
missing command. It imports py1337x, runs a search or resolves a torrent page,
and prints the same shape Vyzor already parses:

    {"title": ..., "extractor": "1337x",
     "entries": [{"id", "title", "url", "uploader", "duration", "thumbnail"}, ...]}

Each entry's `url` is the **magnet link**, not the torrent page: the magnet is
what the download path fetches with aria2c, and a magnet is stable where a page
URL is a search result that can be reordered. `id` is the info-hash.

Cloudflare: 1337x sits behind a JS challenge and answers a plain `requests`
client with 403. When `curl_cffi` is importable this wrapper routes the
library's HTTP through it with a browser TLS fingerprint, which is the same
impersonation Vyzor uses elsewhere. It is not a guarantee — a datacenter IP is
often refused whatever the fingerprint — but it is strictly better than plain
requests, and it degrades to requests when curl_cffi is absent.
"""

import argparse
import json
import re
import sys
import urllib.parse

# Bound the work: py1337x's own search resolves *every* row it finds (one page
# fetch per torrent), so a cap keeps one call from turning into dozens.
DEFAULT_LIMIT = 10
MAX_LIMIT = 50


def _install_impersonation(proxy):
    """Route `requests` through curl_cffi's browser fingerprint, if available.

    py1337x calls `requests.get` directly, so the only way to give it a real
    TLS fingerprint is to replace that function. Done once, here, before the
    library is used. Returns the transport actually in use, for the log line.
    """
    try:
        from curl_cffi import requests as cffi
    except Exception:
        return "requests"

    import requests as std

    def get(url, headers=None, timeout=None, **kw):
        for imp in ("chrome", "chrome131", "safari"):
            try:
                return cffi.get(url, headers=headers, impersonate=imp,
                                proxies={"http": proxy, "https": proxy} if proxy else None,
                                timeout=timeout or 30)
            except Exception:
                continue
        return std.get(url, headers=headers, timeout=timeout or 30)

    std.get = get
    return "curl_cffi"


def _magnet_hash(magnet):
    """The btih info-hash from a magnet URI, or ''."""
    m = re.search(r"btih:([0-9a-fA-F]{40}|[A-Za-z2-7]{32})", magnet or "")
    return m.group(1).lower() if m else ""


def _entry_from_torrent(t):
    """One py1337x torrent dict -> one yt-dlp-shaped entry."""
    magnet = t.get("MagnetLink") or ""
    title = t.get("title") or ""
    entry = {
        "id": _magnet_hash(magnet) or None,
        "title": title or None,
        # The magnet, because that is what the download path fetches.
        "url": magnet or None,
        "uploader": None,
        "duration": None,
        "thumbnail": None,
    }
    return entry


def _search(api, term, limit, site):
    """Search the index and return a yt-dlp-shaped document."""
    # smallToLargeTorrents=False keeps the index's own relevance order; the size
    # sort would bury the best-seeded match at the bottom of a large result set.
    rows = api.search(term, smallToLargeTorrents=False)
    entries = [_entry_from_torrent(t) for t in rows[:limit]]
    entries = [e for e in entries if e["url"]]
    return {
        "title": term,
        "extractor": site,
        "entries": entries,
    }


def _resolve_page(api, url, site):
    """A single torrent page -> one entry (a magnet)."""
    # py1337x wants the path relative to its base; the page URL carries it.
    path = urllib.parse.urlparse(url).path
    data = api.get_torrent_data(path)
    if not data:
        return {"title": None, "extractor": site, "entries": []}
    return {
        "title": data.get("title"),
        "extractor": site,
        "entries": [_entry_from_torrent(data)],
    }


def _term_from_search_url(url):
    """The search term out of a 1337x search URL, or None.

    `https://1337x.to/search/<term>/1/` and `/sort-search/<term>/.../` both carry
    the term as a path segment. Vyzor's crawl is given a *page* URL, so this is
    how a crawl of a search page becomes a search.
    """
    path = urllib.parse.urlparse(url).path
    for marker in ("/search/", "/sort-search/"):
        if marker in path:
            rest = path.split(marker, 1)[1]
            seg = rest.split("/", 1)[0]
            return urllib.parse.unquote_plus(seg) or None
    return None


def main(argv):
    ap = argparse.ArgumentParser(description="py1337x as a JSON CLI")
    ap.add_argument("--version", action="store_true")
    ap.add_argument("--json", action="store_true",
                    help="print one JSON object (the only supported output)")
    ap.add_argument("--site", default="1337x")
    ap.add_argument("--limit", type=int, default=DEFAULT_LIMIT)
    ap.add_argument("--proxy", default=None)
    ap.add_argument("query", nargs="?", default=None,
                    help="a search term, or a 1337x page URL")
    args = ap.parse_args(argv)

    if args.version:
        # The availability probe runs `<tool> --version` and wants exit 0.
        print("py1337x-json 0.1 (py1337x)")
        return 0

    if args.site and args.site.lower() != "1337x":
        print(f"py1337x-json: unknown site {args.site!r}; only '1337x' is supported",
              file=sys.stderr)
        return 2

    if not args.query:
        print("py1337x-json: no query or URL given", file=sys.stderr)
        return 2

    transport = _install_impersonation(args.proxy)

    try:
        from Py1337x import Py1337x
    except Exception as exc:  # noqa: BLE001 - reported, not swallowed
        print(f"py1337x-json: py1337x is not importable ({exc})", file=sys.stderr)
        return 3

    limit = max(1, min(args.limit, MAX_LIMIT))
    api = Py1337x()

    try:
        if args.query.startswith("http://") or args.query.startswith("https://"):
            term = _term_from_search_url(args.query)
            if term:
                doc = _search(api, term, limit, args.site)
            else:
                doc = _resolve_page(api, args.query, args.site)
        else:
            doc = _search(api, args.query, limit, args.site)
    except Exception as exc:  # noqa: BLE001
        # A non-zero exit is how Vyzor learns the tool failed; say why on stderr.
        print(f"py1337x-json: {type(exc).__name__}: {exc} (transport={transport})",
              file=sys.stderr)
        return 1

    # A search that returns nothing is a *result*, not an error — Vyzor treats an
    # empty document as "resolved, nothing here".
    json.dump(doc, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
