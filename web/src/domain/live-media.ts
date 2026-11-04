/**
 * What a live page has loaded, as the console sees it.
 *
 * The engine watches the browser's own network activity, so an entry here is
 * something the page really fetched — a real URL, a real size, a real content
 * type — rather than a reference found in the markup. That distinction matters
 * for a stream: the manifest and its variants are not in the HTML at all.
 *
 * The types are the console's own, not the wire's, so a wire change stops at the
 * boundary (see `domain/wire.ts` for the shapes as they arrive).
 */

import type { WireLiveMedia, WireLiveMediaVariant } from './wire';

/** What an entry is, which is how the list groups it. */
export type LiveMediaKind = 'video' | 'audio' | 'image' | 'manifest';

/** One quality behind a manifest. */
export interface LiveMediaVariant {
  /** "1080p", or a bitrate when the playlist did not say a resolution. */
  label: string;
  width: number;
  height: number;
  bandwidth: number;
  /** The variant's own playlist, which is what a download would use. */
  url: string;
}

/** One thing the page loaded that can be downloaded. */
export interface LiveMediaItem {
  url: string;
  kind: LiveMediaKind;
  /** The Content-Type the server sent, without its parameters. May be empty. */
  mime: string;
  /** Bytes the browser actually received. -1 when the response had no length,
   *  which is normal for a live stream. */
  bytes: number;
  /** A playlist or an MPD: one URL, several qualities. */
  manifest: boolean;
  /** The qualities of a manifest, when they were read. */
  variants: LiveMediaVariant[];
  /** The name a save should use, derived from the URL. */
  filename: string;
}

const KINDS: ReadonlyArray<LiveMediaKind> = ['video', 'audio', 'image', 'manifest'];

/**
 * Read one media message.
 *
 * Tolerant by design: the list is display data, and a malformed entry should be
 * skipped rather than taking the whole list with it — a page that loaded a
 * hundred things should still show the ninety-nine that parsed.
 */
export function normalizeLiveMedia(w: { items?: WireLiveMedia[] }): LiveMediaItem[] {
  const raw = Array.isArray(w?.items) ? w.items : [];
  const out: LiveMediaItem[] = [];
  for (const it of raw) {
    if (!it || typeof it.url !== 'string' || !it.url) continue;
    const kind = KINDS.includes(it.kind as LiveMediaKind)
      ? (it.kind as LiveMediaKind)
      : 'video';
    out.push({
      url: it.url,
      kind,
      mime: typeof it.mime === 'string' ? it.mime : '',
      bytes: typeof it.bytes === 'number' ? it.bytes : -1,
      manifest: it.manifest === true,
      variants: normalizeVariants(it.variants),
      filename: filenameFromUrl(it.url),
    });
  }
  return out;
}

function normalizeVariants(raw: WireLiveMediaVariant[] | undefined): LiveMediaVariant[] {
  if (!Array.isArray(raw)) return [];
  const out: LiveMediaVariant[] = [];
  for (const v of raw) {
    if (!v || typeof v.url !== 'string') continue;
    out.push({
      label: typeof v.label === 'string' && v.label ? v.label : 'variant',
      width: typeof v.width === 'number' ? v.width : 0,
      height: typeof v.height === 'number' ? v.height : 0,
      bandwidth: typeof v.bandwidth === 'number' ? v.bandwidth : 0,
      url: v.url,
    });
  }
  // Best first: a list of qualities is read from the top, and the top should be
  // the one someone usually wants.
  out.sort((a, b) => b.height - a.height || b.bandwidth - a.bandwidth);
  return out;
}

/**
 * A filename from a URL.
 *
 * The last path segment, decoded, with any query dropped — a media URL often
 * carries a token or a range in its query, and none of that belongs in a name.
 * A segment that is not usable falls back to the host, so a name is always
 * something rather than empty.
 */
export function filenameFromUrl(url: string): string {
  let path = url;
  try {
    const u = new URL(url);
    path = u.pathname;
    if (!path || path === '/') return u.hostname || 'download';
  } catch {
    // Not a URL the parser accepts; fall back to string work below.
  }
  const last = path.split('/').filter(Boolean).pop() ?? '';
  let name = last;
  try {
    name = decodeURIComponent(last);
  } catch {
    /* a percent-escape that is not valid; keep it as it came */
  }
  /*
   * A name has to look like one.
   *
   * A string that is not a URL at all — a bare word, a fragment — would
   * otherwise be handed back whole. The check is on the *path* rather than on
   * the decoded name, because a legitimate name can contain a space once its
   * percent-escape is decoded, and rejecting that would throw away a good name.
   */
  if (!name) return 'download';
  if (!path.includes('/')) return 'download';
  return name;
}

/** A byte count as a person reads it. -1 means the source did not say. */
export function formatMediaSize(bytes: number): string {
  if (bytes < 0) return 'size unknown';
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let n = bytes / 1024;
  let i = 0;
  while (n >= 1024 && i < units.length - 1) {
    n /= 1024;
    i++;
  }
  return `${n < 10 ? n.toFixed(1) : Math.round(n)} ${units[i]}`;
}

/** A bitrate as a person reads it. 0 means the playlist did not say. */
export function formatBandwidth(bps: number): string {
  if (bps <= 0) return '';
  if (bps >= 1_000_000) return `${(bps / 1_000_000).toFixed(1)} Mbps`;
  return `${Math.round(bps / 1000)} kbps`;
}

/** What to call an entry in a list: its format, then its kind. */
export function mediaFormatLabel(item: LiveMediaItem): string {
  if (item.manifest) return item.mime.includes('dash') ? 'DASH' : 'HLS';
  const ext = item.filename.includes('.') ? item.filename.split('.').pop()! : '';
  if (ext) return ext.toUpperCase();
  if (item.mime) return item.mime.split('/').pop()!.toUpperCase();
  return item.kind.toUpperCase();
}
