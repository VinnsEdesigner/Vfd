/**
 * Where browse results come from.
 *
 * Deliberately NOT routed through the engine's transport. That transport prefixes
 * the engine's base URL and attaches its bearer token to every request, so using
 * it here would send the user's engine credentials to a third-party host. This
 * source talks to its provider directly, with no credentials at all.
 *
 * Wikimedia Commons is the provider because it is open, needs no API key, sends
 * `Access-Control-Allow-Origin: *` (verified against the live endpoint), and its
 * media is public domain or freely licensed — which is the only kind of material
 * this app is for.
 */

import type { ApiClient } from '../api-client';
import { normalizeBrowseResult } from '../domain/normalize';
import { browseKindFor, type BrowseItem, type BrowseKind, type BrowseQuery, type BrowseResult } from '../domain/browse';
import type { WireBrowseResult } from '../domain/wire';

const API = 'https://commons.wikimedia.org/w/api.php';

/** Commons search filter per kind. `text` has no Commons equivalent. */
const FILETYPE: Partial<Record<BrowseKind, string>> = {
  image: 'bitmap',
  audio: 'audio',
  video: 'video',
  document: 'pdf',
};

/** Kinds this source can actually find. `text` is not one of them. */
export const SUPPORTED_KINDS: ReadonlyArray<BrowseKind | 'all'> = [
  'all',
  'image',
  'audio',
  'video',
  'document',
];

interface CommonsImageInfo {
  url?: string;
  descriptionurl?: string;
  mime?: string;
  size?: number;
  width?: number;
  height?: number;
  thumburl?: string;
  duration?: number;
  extmetadata?: Record<string, { value?: unknown }>;
}

interface CommonsPage {
  pageid?: number;
  title?: string;
  imageinfo?: CommonsImageInfo[];
}

interface CommonsResponse {
  query?: { pages?: Record<string, CommonsPage>; searchinfo?: { totalhits?: number } };
  continue?: { gsroffset?: number };
}

/** Title without its `File:` prefix or trailing extension. */
function titleFrom(raw: string): string {
  const noPrefix = raw.replace(/^File:/i, '');
  return noPrefix.replace(/\.[a-z0-9]{1,5}$/i, '') || noPrefix;
}

/** The filename, with its extension, that the raw title carries. */
function filenameFrom(raw: string): string {
  return raw.replace(/^File:/i, '') || 'download';
}

/** A metadata value as a plain string, with any HTML stripped. */
function meta(info: CommonsImageInfo, key: string): string {
  const v = info.extmetadata?.[key]?.value;
  if (typeof v !== 'string') return '';
  return v.replace(/<[^>]*>/g, '').trim();
}

function toItem(page: CommonsPage, source: string): BrowseItem | null {
  const info = page.imageinfo?.[0];
  if (!info?.url || !page.title) return null;
  const filename = filenameFrom(page.title);
  const mime = info.mime ?? '';
  return {
    id: String(page.pageid ?? info.url),
    title: titleFrom(page.title),
    source,
    pageUrl: info.descriptionurl ?? null,
    kind: browseKindFor(mime, filename),
    mime,
    filename,
    bytes: typeof info.size === 'number' ? info.size : -1,
    url: info.url,
    thumbUrl: info.thumburl ?? null,
    licence: meta(info, 'LicenseShortName') || meta(info, 'UsageTerms') || 'See source',
    width: typeof info.width === 'number' ? info.width : null,
    height: typeof info.height === 'number' ? info.height : null,
    durationSeconds: typeof info.duration === 'number' ? info.duration : null,
  };
}

export interface BrowseSourceOptions {
  /** Injectable for tests; defaults to the platform fetch. */
  fetchImpl?: typeof fetch;
  signal?: AbortSignal;
}

/**
 * List the media a page or directory offers, by asking the engine.
 *
 * This has to go through the engine rather than the browser: the browser
 * enforces CORS, and most media hosts send no CORS headers, so a direct fetch
 * from the page is blocked. The engine has no such limit. It also means the
 * user's engine credentials are used for the request, which is correct — the
 * engine is the thing allowed to fetch on their behalf.
 */
export function createBrowseResource(client: ApiClient) {
  return {
    async listFromUrl(url: string, signal?: AbortSignal): Promise<BrowseResult> {
      const wire = await client.transport.request<WireBrowseResult>({
        method: 'POST',
        path: '/api/v1/browse',
        body: { url },
        ...(signal ? { signal } : {}),
      });
      return normalizeBrowseResult(wire);
    },
  };
}

export type BrowseResource = ReturnType<typeof createBrowseResource>;

/**
 * Search Commons. Returns candidates only — nothing is downloaded here.
 */
export async function searchCommons(
  query: BrowseQuery,
  options: BrowseSourceOptions = {},
): Promise<BrowseResult> {
  const term = query.term.trim();
  if (!term) return { items: [], total: 0, hasMore: false };

  const filter = query.kind !== 'all' ? FILETYPE[query.kind] : undefined;
  // A kind Commons cannot filter for would otherwise silently return everything.
  if (query.kind !== 'all' && !filter) {
    return { items: [], total: 0, hasMore: false };
  }

  const params = new URLSearchParams({
    action: 'query',
    generator: 'search',
    gsrsearch: filter ? `filetype:${filter} ${term}` : term,
    gsrnamespace: '6', // the File: namespace
    gsrlimit: String(query.perPage),
    gsroffset: String(Math.max(0, (query.page - 1) * query.perPage)),
    prop: 'imageinfo',
    iiprop: 'url|size|mime|extmetadata|dimensions',
    // Ask for a grid-sized render so the results view is not loading originals.
    iiurlwidth: '360',
    format: 'json',
    origin: '*', // required by MediaWiki for a cross-origin browser call
  });

  const fetcher = options.fetchImpl ?? globalThis.fetch;
  const res = await fetcher(`${API}?${params.toString()}`, {
    method: 'GET',
    ...(options.signal ? { signal: options.signal } : {}),
  });
  if (!res.ok) throw new Error(`the media search returned HTTP ${res.status}`);

  const body = (await res.json()) as CommonsResponse;
  const pages = Object.values(body.query?.pages ?? {});
  const items = pages
    .map((p) => toItem(p, 'Wikimedia Commons'))
    .filter((i): i is BrowseItem => i !== null);

  const total = body.query?.searchinfo?.totalhits ?? -1;
  // The API hands back a continuation token when more results exist; that is a
  // more reliable signal than comparing counts.
  const hasMore = Boolean(body.continue?.gsroffset);
  return { items, total, hasMore };
}
