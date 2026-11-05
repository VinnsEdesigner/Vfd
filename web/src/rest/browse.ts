/**
 * Where browse results come from.
 *
 * Deliberately NOT routed through Vyzor's transport. That transport prefixes
 * Vyzor's base URL and attaches its bearer token to every request, so using
 * it here would send the user's Vyzor credentials to a third-party host. This
 * source talks to its provider directly, with no credentials at all.
 *
 * Wikimedia Commons is the provider because it is open, needs no API key, sends
 * `Access-Control-Allow-Origin: *` (verified against the live endpoint), and its
 * media is public domain or freely licensed — which is the only kind of material
 * this app is for.
 */

import { isApiError, type ApiClient } from '../api-client';
import { normalizeBrowseResult, normalizeLiveMessage } from '../domain/normalize';
import {
  browseKindFor,
  type BrowseFilter,
  type BrowseItem,
  type BrowseKind,
  type BrowseQuery,
  type BrowseResult,
  type FetchedPage,
  type LiveFailure,
  type LiveFrame,
  type LiveMessage,
  type RenderFailure,
  type RenderMode,
} from '../domain/browse';
import type { WireBrowseResult, WireLiveImageHeader, WireLiveMessage } from '../domain/wire';

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
 * List the media a page or directory offers, by asking Vyzor.
 *
 * This has to go through Vyzor rather than the browser: the browser
 * enforces CORS, and most media hosts send no CORS headers, so a direct fetch
 * from the page is blocked. Vyzor has no such limit. It also means the
 * user's Vyzor credentials are used for the request, which is correct — the
 * Vyzor is the thing allowed to fetch on their behalf.
 */
export function createBrowseResource(client: ApiClient) {
  return {
    async listFromUrl(
      url: string,
      filter: BrowseFilter = 'media',
      signal?: AbortSignal,
    ): Promise<BrowseResult> {
      const wire = await client.transport.request<WireBrowseResult>({
        method: 'POST',
        path: '/api/v1/browse',
        body: { url, filter },
        ...(signal ? { signal } : {}),
      });
      return normalizeBrowseResult(wire);
    },
  };
}

export type BrowseResource = ReturnType<typeof createBrowseResource>;

/**
 * The URL the console embeds to show a page, and whether the site allows it.
 *
 * The bytes are served by Vyzor — the console cannot fetch another origin
 * itself — and Vyzor reports the site's framing policy in a header, so a
 * refusal can be explained before the frame is shown empty.
 */
export function createPageResource(client: ApiClient) {
  return {
    /** The URL to point an <iframe> at, for a given site. */
    frameUrl(url: string): string {
      return client.transport.resolve(
        `/api/v1/page?url=${encodeURIComponent(url)}`,
      );
    },

    /** Ask Vyzor to fetch the page and report its framing policy. */
    async inspect(url: string, signal?: AbortSignal): Promise<FetchedPage> {
      const headers = await client.transport.head({
        method: 'GET',
        path: `/api/v1/page?url=${encodeURIComponent(url)}`,
        ...(signal ? { signal } : {}),
      });
      // Only the headers are wanted here: the body is the page itself, which
      // the iframe fetches separately.
      return {
        url,
        framable: headers['x-vyzorix-framable'] !== 'no',
        blockedBy: headers['x-vyzorix-frame-blocker'] ?? null,
      };
    },
  };
}

export type PageResource = ReturnType<typeof createPageResource>;

/**
 * Rendering a page with Vyzor's browser.
 *
 * The image is not fetched here: the console points an <img> at Vyzor's
 * render URL, so the browser streams and caches it the way it does any other
 * image, and a large render never passes through JS memory.
 *
 * What *is* asked for is the outcome, because a render can fail in ways a
 * broken image cannot explain — the gate may be full, the page may never
 * settle, the URL may be one we refuse. A header-only request reads that.
 */
export function createRenderResource(client: ApiClient) {
  const path = (url: string, mode: RenderMode) =>
    `/api/v1/render?url=${encodeURIComponent(url)}&mode=${mode}`;

  return {
    /** Where a rendered page is served from, for a link or a new tab. */
    url(url: string, mode: RenderMode = 'image'): string {
      return client.transport.resolve(path(url, mode));
    },

    /**
     * Render a page and return a URL the browser can display.
     *
     * The bytes are fetched *here* rather than by pointing an `<img>` at the
     * Vyzor, because an image request carries no Authorization header: it would
     * work on an open Vyzor and 401 on a secured one, silently showing a broken
     * image. Fetching with the same auth as every other call, then handing over a
     * blob, makes the two cases identical.
     *
     * The outcome comes from the response headers, so a failure is a specific
     * reason rather than an image that never loads.
     */
    async render(url: string, mode: RenderMode = 'image', signal?: AbortSignal) {
      try {
        const res = await client.transport.fetchWithHeaders({
          method: 'GET',
          path: path(url, mode),
          ...(signal ? { signal } : {}),
        });
        const objectUrl = URL.createObjectURL(res.blob);
        return {
          ok: true as const,
          /** The blob URL to display. The caller must revoke it when done. */
          objectUrl,
          status: res.headers['x-vyzorix-render'] ?? 'ok',
          title: res.headers['x-vyzorix-title'] ?? null,
          elapsedMs: Number(res.headers['x-vyzorix-render-ms'] ?? 0),
          truncated: res.headers['x-vyzorix-render-truncated'] === 'yes',
          /** True when Vyzor served this from its cache rather than
           *  rendering again, so the UI can say why it was instant. */
          fromCache: res.headers['x-vyzorix-render-cache'] === 'hit',
          /**
           * True when Vyzor stripped the page's scripts, handlers and
           * frames before serving it. Worth saying, because "Page text" looks
           * like the site and is not: it is a cleaned copy of it.
           */
          sanitized: res.headers['x-vyzorix-sanitized'] === 'yes',
        };
      } catch (e) {
        return { ok: false as const, failure: renderFailureFor(e) };
      }
    },
  };
}

export type RenderResource = ReturnType<typeof createRenderResource>;

/**
 * A live, driveable page session.
 *
 * Unlike every other resource here, this one is a WebSocket rather than a
 * request: Vyzor streams image frames down and the console sends input
 * events up. `open` hands back a small handle that owns the socket and decodes
 * both directions into the domain.
 *
 * The token cannot travel as a header — a browser's WebSocket API sets none — so
 * it goes in the query string, which is why Vyzor accepts it there and
 * checks the Origin in exchange.
 */
export function createLiveResource(client: ApiClient) {
  return {
    /** The ws:// URL for a session, for a caller that wants to open it itself. */
    url(url: string, width: number, height: number, touch = false): string {
      return client.transport.socketUrl('/api/v1/interactive', {
        url,
        w: Math.round(width),
        h: Math.round(height),
        ...(touch ? { touch: 1 } : {}),
      });
    },

    /**
     * Open a session and start decoding it.
     *
     * The callbacks are called from the socket's own event handlers; the caller
     * is responsible for revoking frame blob URLs when it replaces them, which
     * `onFrame` is told about through the `LiveFrame` it receives.
     *
     * `touch` tells Vyzor to emulate a touch device, so the page lays itself
     * out the way it would on this device and a forwarded finger behaves as one.
     */
    open(
      url: string,
      width: number,
      height: number,
      handlers: {
        onMessage?: (m: LiveMessage) => void;
        onFrame?: (f: LiveFrame) => void;
        onOpen?: () => void;
        onClose?: (failure: LiveFailure, reason: string) => void;
      },
      touch = false,
    ): LiveSession {
      return new LiveSession(client.transport.socketUrl('/api/v1/interactive', {
        url,
        w: Math.round(width),
        h: Math.round(height),
        ...(touch ? { touch: 1 } : {}),
      }), handlers);
    },
  };
}

export type LiveResource = ReturnType<typeof createLiveResource>;

/** The socket states a session reports. */
export type LiveStatus = 'connecting' | 'open' | 'closed';

/**
 * One live session.
 *
 * Owns a WebSocket and a small state machine over Vyzor's two message
 * kinds: JSON text (ready/state/closed/error and image headers) and binary
 * (the image itself). A binary frame is paired with the header that preceded it
 * so the caller gets one `LiveFrame` rather than two callbacks.
 */
/** What a live session's stream is actually doing, measured rather than assumed. */
export interface LiveStreamStats {
  /** Frames received and decoded. */
  received: number;
  /** The highest `seq` seen — Vyzor's own count of frames it produced. */
  highestSeq: number;
  /**
   * Frames Vyzor produced but this client never saw.
   *
   * A gap in `seq` is a frame Vyzor dropped to keep the view current, or
   * one lost in transit. Either way it is the number that says whether the link
   * is keeping up.
   */
  skipped: number;
  /** Milliseconds from a frame's bytes arriving to the previous frame painting. */
  lastGapMs: number;
  /** The largest such gap seen, which is the stall a user would notice. */
  worstGapMs: number;
  /** Milliseconds since the previous frame arrived, for the live rate. */
  lastArrivalMs: number;
}

export class LiveSession {
  private socket: WebSocket | null = null;
  /** The header of a frame whose binary payload has not arrived yet. */
  private pendingHeader: WireLiveImageHeader | null = null;
  private status: LiveStatus = 'connecting';
  /** A failure captured before close, so the close callback can name it. */
  private failure: LiveFailure | null = null;
  /**
   * The application-level heartbeat, and the visibility listeners that keep it
   * meaningful.
   *
   * Vyzor gives up on a client it has not heard from, and the browser's own
   * answer to a protocol ping is not a reliable sign of life: a backgrounded tab
   * throttles its timers, and a phone that sleeps suspends the socket entirely.
   * So the console says "still here" itself, and when the tab comes back to the
   * foreground it says so at once rather than waiting for the next tick — which
   * is exactly the moment Vyzor is most likely to have stopped hearing.
   */
  private heartbeat: number | null = null;
  private onVisibility: (() => void) | null = null;
  /** The interval between heartbeats. Well inside Vyzor's silence window. */
  private static readonly HEARTBEAT_MS = 20_000;
  /**
   * What the stream is doing, measured.
   *
   * These numbers exist because the latency question could not be answered by
   * reasoning: Vyzor measures microseconds to produce a frame, so if the
   * user sees seconds the delay is either in the transport or in the paint. The
   * `seq` gap separates those — a gap means frames were dropped to keep up (a
   * link problem), while a growing `worstGapMs` with no gap means each frame
   * took a long time to arrive (also a link problem), and neither with a slow
   * feel would point at the paint instead.
   */
  private stats: LiveStreamStats = {
    received: 0,
    highestSeq: 0,
    skipped: 0,
    lastGapMs: 0,
    worstGapMs: 0,
    lastArrivalMs: 0,
  };
  private lastArrivalAt = 0;

  constructor(
    private readonly wsUrl: string,
    private readonly handlers: {
      onMessage?: (m: LiveMessage) => void;
      onFrame?: (f: LiveFrame) => void;
      onOpen?: () => void;
      onClose?: (failure: LiveFailure, reason: string) => void;
    },
  ) {}

  getStatus(): LiveStatus {
    return this.status;
  }

  connect(): void {
    let socket: WebSocket;
    try {
      socket = new WebSocket(this.wsUrl);
    } catch {
      // A malformed URL or a blocked scheme is a failure to start, not a close.
      this.failure = 'failed';
      this.status = 'closed';
      this.handlers.onClose?.('failed', 'could not open the session socket');
      return;
    }
    socket.binaryType = 'arraybuffer';
    this.socket = socket;

    socket.onopen = () => {
      this.status = 'open';
      this.startHeartbeat();
      this.handlers.onOpen?.();
    };

    socket.onmessage = (ev: MessageEvent) => {
      if (typeof ev.data === 'string') {
        this.onText(ev.data);
      } else if (ev.data instanceof ArrayBuffer) {
        this.onBinary(ev.data);
      }
    };

    socket.onerror = () => {
      // An error is followed by a close; the close handler reports it.
      if (!this.failure) this.failure = 'failed';
    };

    socket.onclose = (ev: CloseEvent) => {
      this.status = 'closed';
      this.stopHeartbeat();
      // A 1000 close is deliberate; anything else, or a socket that never
      // opened, is a failure worth naming. Vyzor reports the specific
      // reason (busy, refused, disabled) as an HTTP status on the handshake,
      // which the browser surfaces only as a close code, so the mapping here is
      // coarse on purpose.
      const failure = this.failure ?? (ev.wasClean ? 'closed' : 'failed');
      this.handlers.onClose?.(failure, failure === 'closed' ? 'the session closed' : 'the session could not start');
    };
  }

  private onText(data: string): void {
    let w: WireLiveMessage;
    try {
      w = JSON.parse(data) as WireLiveMessage;
    } catch {
      return; // a message we cannot read is skipped, not fatal
    }
    // A frame/shot header introduces the binary that follows.
    if (w.type === 'frame' || w.type === 'shot') {
      this.pendingHeader = {
        type: w.type,
        format: w.format ?? 'jpeg',
        width: w.width ?? 0,
        height: w.height ?? 0,
        seq: w.seq ?? 0,
      };
      return;
    }
    const msg = normalizeLiveMessage(w);
    if (msg) {
      if (msg.kind === 'closed') this.failure = 'closed';
      if (msg.kind === 'error') this.failure = 'failed';
      this.handlers.onMessage?.(msg);
    }
  }

  private onBinary(data: ArrayBuffer): void {
    const header = this.pendingHeader;
    this.pendingHeader = null;

    /*
     * Measure before doing anything else, so the number describes the transport
     * rather than this handler's own work.
     */
    const now = typeof performance !== 'undefined' ? performance.now() : Date.now();
    if (this.lastArrivalAt > 0) {
      const gap = now - this.lastArrivalAt;
      this.stats.lastArrivalMs = gap;
      if (gap > this.stats.worstGapMs) this.stats.worstGapMs = gap;
    }
    this.lastArrivalAt = now;
    this.stats.received++;
    if (header && header.seq > 0) {
      if (this.stats.highestSeq > 0 && header.seq > this.stats.highestSeq + 1) {
        /* A gap means Vyzor dropped a frame to keep up, or one was lost. */
        this.stats.skipped += header.seq - this.stats.highestSeq - 1;
      }
      this.stats.highestSeq = Math.max(this.stats.highestSeq, header.seq);
    }

    const blob = new Blob([data], { type: header?.format === 'png' ? 'image/png' : 'image/jpeg' });
    const frame: LiveFrame = {
      url: URL.createObjectURL(blob),
      still: header?.type === 'shot',
      width: header?.width ?? 0,
      height: header?.height ?? 0,
      seq: header?.seq ?? 0,
    };
    this.handlers.onFrame?.(frame);
  }

  /**
   * Say "still here" to Vyzor, and keep saying it.
   *
   * The message is a no-op on Vyzor's side; what matters is that bytes
   * arrive, because Vyzor's silence window is measured against *any* bytes
   * received, not against a specific answer. `setInterval` is throttled in a
   * background tab (to roughly once a minute) but not stopped, which is enough:
   * Vyzor's window is minutes, not seconds.
   */
  private startHeartbeat(): void {
    this.stopHeartbeat();
    this.heartbeat = window.setInterval(() => {
      this.send({ type: 'ping' });
    }, LiveSession.HEARTBEAT_MS);
    /*
     * A tab that returns to the foreground has, by definition, just been away.
     * Sending a heartbeat now closes the gap immediately instead of leaving the
     * Vyzor to decide, on its own clock, that the client is gone.
     */
    if (typeof document !== 'undefined') {
      this.onVisibility = () => {
        if (document.visibilityState === 'visible') this.send({ type: 'ping' });
      };
      document.addEventListener('visibilitychange', this.onVisibility);
    }
  }

  private stopHeartbeat(): void {
    if (this.heartbeat !== null) {
      window.clearInterval(this.heartbeat);
      this.heartbeat = null;
    }
    if (this.onVisibility && typeof document !== 'undefined') {
      document.removeEventListener('visibilitychange', this.onVisibility);
    }
    this.onVisibility = null;
  }

  /** What the stream is doing. A copy, so a caller cannot disturb it. */
  streamStats(): LiveStreamStats {
    return { ...this.stats };
  }

  /** Send a command to Vyzor. A no-op once the socket is gone. */
  send(message: unknown): void {
    if (this.socket && this.socket.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(message));
    }
  }

  close(): void {
    this.stopHeartbeat();
    if (this.socket) {
      // A clean close code, so Vyzor sees a deliberate departure rather
      // than a dropped connection.
      try {
        this.socket.close(1000, 'client closed');
      } catch {
        /* already closing */
      }
      this.socket = null;
    }
    this.status = 'closed';
  }
}

/** Map a failed render request onto the reason the console should show. */
function renderFailureFor(e: unknown): RenderFailure {
  const status = isApiError(e) ? e.status : 0;
  const detail = isApiError(e) ? (e.detail ?? '') : '';
  if (status === 503) {
    // Both "not enabled" and "no browser" are 503, so the detail decides which.
    if (/not enabled/i.test(detail)) return 'disabled';
    if (/no browser/i.test(detail)) return 'unavailable';
    return 'busy';
  }
  if (status === 504) return 'timeout';
  if (status === 400) return 'refused';
  return 'failed';
}

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
