#!/usr/bin/env python3
"""fetch-impersonate — fetch a URL with a real browser's TLS fingerprint.

The engine's own HTTP client is libcurl, which is honest about being libcurl.
Some sites fingerprint the TLS handshake (JA3/JA4) and refuse anything that is
not a browser, no matter what User-Agent it claims — a check that no header can
pass. `curl_cffi` replays a real browser's handshake, so the request looks like
Chrome or Firefox all the way down.

This is a separate process on purpose: the engine stays C and libcurl, and the
one thing it cannot do is delegated to the tool that can. The engine writes no
Python and imports nothing; it runs this and reads the file it produces.

Invoked by the engine as:

    fetch-impersonate --url URL --out PATH [--impersonate chrome]
                      [--timeout SECONDS] [--max-bytes N]
                      [--proxy URL] [--user-agent UA] [--cookies-file PATH]

On success the body is written to --out and the process exits 0. On failure it
exits non-zero and prints a one-line reason to stderr, which the engine logs.
"""

import argparse
import sys


def main() -> int:
    ap = argparse.ArgumentParser(prog="fetch-impersonate")
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--meta-out", default="")
    ap.add_argument("--impersonate", default="chrome")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--max-bytes", type=int, default=0)
    ap.add_argument("--proxy", default="")
    ap.add_argument("--user-agent", default="")
    ap.add_argument("--cookies-file", default="")
    args = ap.parse_args()

    try:
        from curl_cffi import requests
    except Exception as exc:  # noqa: BLE001 - the message is the point
        print(f"curl_cffi is not installed: {exc}", file=sys.stderr)
        return 3

    # "chrome" is a family; curl_cffi wants a concrete version. If the operator
    # named a family, take the newest the installed curl_cffi knows, so the
    # helper keeps working as curl_cffi adds versions.
    target = args.impersonate
    if target in ("chrome", "firefox", "safari", "edge"):
        target = _newest(target)
    elif target == "":  # an empty string means "any supported browser"
        target = _newest("chrome")

    headers = {}
    if args.user_agent:
        headers["User-Agent"] = args.user_agent
    cookies = _load_cookies(args.cookies_file)

    proxies = {"http": args.proxy, "https": args.proxy} if args.proxy else None

    try:
        resp = requests.get(
            args.url,
            impersonate=target,
            timeout=args.timeout,
            headers=headers or None,
            cookies=cookies or None,
            proxies=proxies,
            allow_redirects=True,
        )
    except Exception as exc:  # noqa: BLE001
        print(f"request failed: {exc}", file=sys.stderr)
        return 4

    if resp.status_code < 200 or resp.status_code >= 300:
        print(f"HTTP {resp.status_code}", file=sys.stderr)
        return 5

    body = resp.content
    if args.max_bytes and len(body) > args.max_bytes:
        body = body[: args.max_bytes]

    try:
        with open(args.out, "wb") as fh:
            fh.write(body)
    except OSError as exc:
        print(f"cannot write {args.out}: {exc}", file=sys.stderr)
        return 6

    # The response headers, so the caller can apply its own framing rule. curl_cffi
    # exposes them as a case-insensitive multimap; flatten to "Name: value" lines
    # exactly as an HTTP client would deliver them, which is what the C side parses.
    if args.meta_out:
        import json

        headers = [f"{k}: {v}" for k, v in resp.headers.items()]
        meta = {"final_url": str(resp.url), "headers": headers}
        try:
            with open(args.meta_out, "w", encoding="utf-8") as fh:
                json.dump(meta, fh)
        except OSError as exc:
            print(f"cannot write {args.meta_out}: {exc}", file=sys.stderr)
            return 7
    return 0


def _newest(family: str) -> str:
    """The newest impersonation target of `family` that curl_cffi supports."""
    try:
        from curl_cffi.requests import BrowserType

        names = [n for n in dir(BrowserType) if not n.startswith("_")]
    except Exception:  # noqa: BLE001
        return family
    matches = sorted(n for n in names if n.startswith(family))
    # Names are family+number (chrome131); the largest number is the newest.
    def ver(name: str) -> int:
        digits = "".join(ch for ch in name[len(family):] if ch.isdigit())
        return int(digits) if digits else 0

    return max(matches, key=ver) if matches else family


def _load_cookies(path: str):
    """A Netscape cookie export as a {name: value} dict, or None."""
    if not path:
        return None
    jar = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("#") or not line.strip():
                    continue
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 7:
                    jar[parts[5]] = parts[6]
    except OSError:
        return None
    return jar or None


if __name__ == "__main__":
    sys.exit(main())
