# Crawling

The crawler answers one question: *"I want the file with this name, on this
site."* The user gives a site and a search term, not a URL to a file. The file
may be several clicks in, and the page that links it may be built by JavaScript.

So a crawl walks the site, follows links, and reports every file whose name
matches the term. It is a **crawler with a filter**, not a downloader: nothing but
HTML is fetched, and the caller gets back the URLs a download would use.

It is reached only over HTTP: `POST /api/v1/crawl` (see [api.md](api.md)). There
is no `crawl` subcommand in the CLI.

## Two ways to read a page

| Mode | How | When |
|---|---|---|
| static | fetch the HTML, read its links | the default; right for most sites |
| rendered | re-read the page through the browser | when the static pass finds nothing |

The browser fallback is the point of the module. A site whose links are assembled
in JavaScript looks empty to a plain fetch, and that is exactly the case a
crawler has to handle rather than report as "not found". It costs a browser, so it
is a fallback, not the first move, and it is bounded by `max_browser_pages`.

The interactive pool is preferred over the renderer for the fallback: a session
can be *driven* — wait for the network to settle, click what needs clicking — and
that is what reaches a listing behind a tab or a "Load more".

## Seeding

The walk is seeded from more than the start page:

- **The start page's links.**
- **Sitemaps**, when `use_sitemap` is on (the default). A sitemap is the site's
  own index of its pages, so it reaches pages no link does. They are read from the
  robots.txt `Sitemap:` directives plus the conventional `/sitemap.xml`.
- **The search form**, when a term is given and the page has one: submitting the
  site's own search is often the shortest path to the file.

## Politeness

A crawl is a guest on someone else's host, so it behaves like one.

- **robots.txt is honoured** by default (`obey_robots`). This is on for a crawl,
  and off for a user's explicit fetch of a URL they chose.
- **Per-host pacing.** The politeness state is shared with the rest of the server,
  so a host's rules and its pace are learned once, not per request.
- **The anti-bot brain** is shared too: a host that is refusing is skipped until
  its cooldown, and the identity that worked is reused rather than every page
  being a fresh gamble.

## Bounds

A crawl is a background job with a human waiting, so it must not run forever.
Every bound has a default and can be overridden per request.

| Bound | Default | Why |
|---|---|---|
| `max_depth` | 2 | 0 reads only the page given; each extra level multiplies requests |
| `max_pages` | 40 | a hard stop on a site with a large index |
| `max_hits` | 50 | once this many match, the walk stops: the caller has what it came for |
| `budget_seconds` | 45 | a wall-clock stop |
| `max_browser_pages` | 3 | the browser fallback is expensive; a JavaScript site hides its listing behind its landing page, not behind every page |

`same_host_only` is **off** by default: a datasheet host commonly links its files
from a CDN on another host, and refusing those would miss the file.

## Dedup

A page is fingerprinted with a SimHash, and a page within `dedup_distance` bits of
one already read is not read again — though its links are still followed, because
they may reach new pages. This matters at scale, where the same content appears
under many URLs (a product at several facets, a page at `?session=` and `?ref=`).

## The shared frontier

By default the queue is in-process. When a run id is given and a frontier is
configured, the queue lives in a database instead, so several crawlers walk one
site once rather than once each:

- `--frontier-db PATH` — a local SQLite file. Self-contained, no account, no
  network. A crawl interrupted by a restart resumes from this file.
- `MI_FRONTIER_DB_URL` + `MI_FRONTIER_TOKEN` — a Turso database, for workers on
  different hosts.

A worker that dies mid-page leaves a claim; a claim older than
`frontier_lease_seconds` (default 300) is assumed abandoned and re-queued.

## The result

The response carries the hits and the counters that explain them:

- `found` and `items[]` — `{url, filename, title, mime, score, found_on,
  via_browser, resolved, thumbnail, qualities[]}`. `score` is how well the name
  matched, so the likely answer can be shown first. `resolved` marks a hit that a
  media extractor resolved, which ranks first at an equal score.
- `pages_read`, `pages_failed`, `pages_rendered`, `skipped_offsite`.
- `challenges_seen` — pages that answered with a captcha wall.
- `blocked` — pages that answered `200` with a wall rather than content. A soft
  block is the failure that looks like success, so it is reported separately.
- `not_modified` — pages the server said were unchanged (`304`).
- `duplicates` — pages skipped as near-duplicates.

These exist so an empty result can be explained: "40 pages read, 12 needed the
browser, none matched" is a different answer from "the host refused us".

## What it does not do

- It does not download. It returns URLs; a download is a separate call.
- It does not resolve a page's media. A streaming page's media URL is not among
  its links, so the extractor step asks yt-dlp about a page and reports the
  *media* rather than the channel it sits on. `use_extractor` (on by default)
  controls this.
- It does not crawl past a sign-in. Supply cookies; the walk and the download
  then agree on the credential.
