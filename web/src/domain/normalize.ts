/**
 * Wire -> domain mapping. This is the only place that knows both shapes, so a
 * server-side rename is a one-file change.
 */

import type {
  SystemStorage,
  Capabilities,
  Conversion,
  ConversionState,
  Download,
  DownloadState,
  Media,
  MediaKind,
  ProbeResult,
  StorageConfig,
  EgressSettings,
} from './media';
import {
  browseKindFor,
  type BrowseItem,
  type BrowseResult,
  type CrawlQuality,
  type CrawlResult,
  type ExtractResult,
  type LiveMessage,
} from './browse';
import type {
  WireBrowseResult,
  WireCrawlResult,
  WireExtractResult,
  WireEgressSettings,
  WireSystemStorage,
  WireCapabilities,
  WireDownload,
  WireDownloadList,
  WireLiveMessage,
  WireMedia,
  WireProbeResult,
  WireStorageConfig,
} from './wire';

/** The host of a URL, for naming where a result came from. The domain's own
 *  copy, because `domain` may not import the hooks layer that formats this. */
function hostOf(url: string): string {
  try {
    return new URL(url).host || url;
  } catch {
    return url;
  }
}

const MEDIA_KINDS: ReadonlySet<string> = new Set<MediaKind>([
  'mp4', 'matroska', 'avi', 'flv', 'ogg', 'asf', 'mpeg-ts', 'mpeg-ps', 'hls', 'dash',
  'jpeg', 'png', 'gif', 'webp', 'bmp', 'tiff', 'avif', 'heic',
  'mp3', 'aac', 'flac', 'wav', 'ogg-audio',
  'unknown',
]);

const JOB_STATES: ReadonlySet<string> = new Set<DownloadState>([
  'queued',
  'running',
  'done',
  'failed',
  'cancelled',
  'paused',
]);

/** Last path segment of a URL, query and fragment stripped. The domain's own
 *  copy of the server's rule, so a server that omits `name` still yields a real
 *  name. A URL that ends at a separator has no filename, so it says "download"
 *  rather than falling back to the host. */
export function nameFromUrl(url: string): string {
  const withoutQuery = (url.split('?')[0] ?? url).split('#')[0] ?? url;
  const end = withoutQuery.length;
  if (end === 0 || withoutQuery[end - 1] === '/') return 'download';
  const segment = withoutQuery.slice(withoutQuery.lastIndexOf('/') + 1);
  if (!segment) return 'download';
  try {
    return decodeURIComponent(segment);
  } catch {
    return segment;
  }
}

export function toMediaKind(raw: string | null | undefined): MediaKind {
  if (raw && MEDIA_KINDS.has(raw)) return raw as MediaKind;
  return 'unknown';
}

export function toDownloadState(raw: string | null | undefined): DownloadState {
  if (raw && JOB_STATES.has(raw)) return raw as DownloadState;
  return 'failed';
}

const CONVERSION_STATES: ReadonlySet<string> = new Set<ConversionState>([
  'none',
  'converting',
  'done',
  'failed',
]);

/** Absent on servers that predate conversion, so it reads as "not asked for". */
export function normalizeConversion(
  w: { state: string; error: string | null } | null | undefined,
): Conversion {
  const state = w && CONVERSION_STATES.has(w.state) ? (w.state as ConversionState) : 'none';
  return { state, error: w?.error ?? null };
}

export function normalizeMedia(w: WireMedia | null | undefined): Media | null {
  if (!w) return null;
  return {
    kind: toMediaKind(w.kind),
    container: w.container ?? '',
    confidence: typeof w.confidence === 'number' ? w.confidence : 0,
    isVideo: Boolean(w.is_video),
    isImage: Boolean(w.is_image),
    isAudio: Boolean(w.is_audio),
    width: w.width ?? 0,
    height: w.height ?? 0,
    qualities: Array.isArray(w.qualities)
      ? w.qualities.map((q) => ({
          width: q.width ?? 0,
          height: q.height ?? 0,
          bandwidth: q.bandwidth ?? -1,
          bytes: q.bytes ?? -1,
          codecs: q.codecs ?? '',
          url: q.url ?? '',
        }))
      : [],
  };
}

export function normalizeDownload(w: WireDownload): Download {
  const name = w.name && w.name.trim() ? w.name : nameFromUrl(w.url);
  return {
    id: w.id,
    name,
    // Older servers sent only `name`, which may lack an extension. Fall back to
    // it so nothing regresses; the extension is derived from the media kind
    // downstream when it is missing.
    filename: w.filename && w.filename.trim() ? w.filename : name,
    url: w.url,
    state: toDownloadState(w.state),
    progress: {
      downloadedBytes: w.progress?.downloaded_bytes ?? 0,
      totalBytes: w.progress?.total_bytes ?? 0,
    },
    bytesPerSecond: w.progress?.bytes_per_second ?? 0,
    media: normalizeMedia(w.media),
    bytes: w.bytes ?? 0,
    sha256: w.sha256 ?? null,
    masterPlaylist: w.master_playlist ?? null,
    conversion: normalizeConversion(w.conversion),
    artifacts: Array.isArray(w.artifacts) ? w.artifacts : [],
    error: w.error ?? null,
    requestedQuality: w.requested_quality ?? null,
    connections: w.connections ?? 0,
    hls: Boolean(w.hls),
    upload: Boolean(w.upload),
    purge: Boolean(w.purge),
    createdAt: w.created_at ?? 0,
    finishedAt: w.finished_at ?? 0,
  };
}

export function normalizeDownloadList(w: WireDownloadList): Download[] {
  return Array.isArray(w.items) ? w.items.map(normalizeDownload) : [];
}

/**
 * A page listing from Vyzor, as candidates.
 *
 * Vyzor has already resolved each reference against the page and kept only
 * media, so this maps names and kinds rather than re-deriving them. The page URL
 * becomes each item's `pageUrl`: that is where a person would go to learn about
 * the file, and it is what the card links to.
 */
export function normalizeBrowseResult(w: WireBrowseResult): BrowseResult {
  const page = w.url ?? '';
  const items = Array.isArray(w.items)
    ? w.items
        .filter((i) => i && typeof i.url === 'string' && i.url)
        .map((i) => {
          const filename = i.filename && i.filename.trim() ? i.filename : nameFromUrl(i.url);
          return {
            id: i.url,
            // The link text is what the page calls it, so it reads better than
            // the filename; fall back to the filename without its extension.
            title: i.title?.trim() || filename.replace(/\.[a-z0-9]{1,5}$/i, '') || filename,
            source: page ? hostOf(page) : 'the web',
            pageUrl: page || null,
            kind: browseKindFor(i.mime ?? '', filename),
            mime: i.mime ?? '',
            filename,
            bytes: typeof i.bytes === 'number' ? i.bytes : -1,
            url: i.url,
            thumbUrl: null,
            licence: '',
            width: null,
            height: null,
            durationSeconds: null,
          } satisfies BrowseItem;
        })
    : [];
  return { items, total: w.count ?? items.length, hasMore: false, title: w.title ?? null };
}

/**
 * A crawl result, as the console uses it.
 *
 * The wire names are snake_case and the counts arrive flat; this groups them so
 * a component asks for `report.pagesRead` rather than remembering which of five
 * sibling fields meant what. Nothing is invented: a field the server did not
 * send becomes a neutral value rather than a guess.
 */
export function normalizeCrawlResult(w: WireCrawlResult): CrawlResult {
  const items = Array.isArray(w.items) ? w.items : [];
  return {
    hits: items.map((i) => ({
      url: i.url,
      filename: i.filename && i.filename.trim() ? i.filename : nameFromUrl(i.url),
      title: i.title && i.title.trim() ? i.title : null,
      mime: i.mime ?? '',
      score: typeof i.score === 'number' ? i.score : 0,
      foundOn: i.found_on ?? null,
      viaBrowser: Boolean(i.via_browser),
      resolved: Boolean(i.resolved),
      thumbnail: i.thumbnail && i.thumbnail.trim() ? i.thumbnail : null,
      qualities: Array.isArray(i.qualities)
        ? i.qualities.map((q) => ({
            height: typeof q.height === 'number' ? q.height : 0,
            label: q.label ?? '',
            bytes: typeof q.bytes === 'number' ? q.bytes : -1,
            best: Boolean(q.best),
          }))
        : [],
    })),
    report: {
      pagesRead: w.pages_read ?? 0,
      pagesFailed: w.pages_failed ?? 0,
      pagesRendered: w.pages_rendered ?? 0,
      skippedOffsite: w.skipped_offsite ?? 0,
    },
    term: w.term ?? '',
    url: w.url ?? '',
  };
}

/**
 * A resolve, as the console uses it.
 *
 * The same call answers a URL and a search, so this returns one shape for both:
 * a single item carries `qualities`, a list carries `entries`. A list resolves no
 * stream URLs, which is why the two are separate fields rather than one union —
 * a caller asking for entries and getting qualities would be reading a list that
 * cost a hundred times what it needed to.
 */
export function normalizeExtractResult(w: WireExtractResult): ExtractResult {
  const formats = Array.isArray(w.formats) ? w.formats : [];
  const entries = Array.isArray(w.entries) ? w.entries : [];

  /*
   * One entry per distinct height, the largest first — the same reduction the
   * crawl does, because the picker is the same picker. A stream lists many
   * variants of one size (a codec each) and showing them all is a wall of
   * near-duplicates.
   */
  const byHeight = new Map<number, CrawlQuality>();
  for (const f of formats) {
    if (!f.has_video || f.height <= 0) continue;
    const existing = byHeight.get(f.height);
    if (existing) {
      if (existing.bytes < 0 && f.filesize > 0) existing.bytes = f.filesize;
      continue;
    }
    byHeight.set(f.height, {
      height: f.height,
      label: f.height >= 2160 ? '4K' : f.height >= 1440 ? '2K' : `${f.height}p`,
      bytes: typeof f.filesize === 'number' ? f.filesize : -1,
      best: false,
    });
  }
  const qualities = [...byHeight.values()].sort((a, b) => b.height - a.height);
  if (qualities.length > 0) qualities[0]!.best = true;

  return {
    url: w.url ?? null,
    title: w.title && w.title.trim() ? w.title : null,
    extractor: w.extractor ?? null,
    durationSeconds: typeof w.duration_seconds === 'number' ? w.duration_seconds : -1,
    thumbnail: w.thumbnail ?? null,
    qualities,
    entries: entries
      .filter((e) => e && (e.title || e.id))
      .map((e) => ({
        id: e.id ?? null,
        // A list entry with no title falls back to its id, so a row is never
        // blank — an untitled video still has one.
        title: e.title && e.title.trim() ? e.title : (e.id ?? 'Untitled'),
        url: e.url ?? '',
        uploader: e.uploader && e.uploader.trim() ? e.uploader : null,
        thumbnail: e.thumbnail ?? null,
        durationSeconds:
          typeof e.duration_seconds === 'number' ? e.duration_seconds : -1,
        isPlaylist: Boolean(e.is_playlist),
      })),
  };
}

export function normalizeProbe(w: WireProbeResult): ProbeResult {
  return {
    url: w.url,
    finalUrl: w.final_url ?? w.url,
    contentType: w.content_type ?? null,
    totalBytes: w.total_bytes ?? -1,
    acceptRanges: Boolean(w.accept_ranges),
    media: normalizeMedia(w.media),
  };
}

export function normalizeCapabilities(w: WireCapabilities): Capabilities {
  const r = w.render;
  return {
    version: w.version ?? '1',
    vyzorVersion: w.vyzor_version ?? null,
    detectors: Array.isArray(w.detectors) ? w.detectors.map(toMediaKind) : [],
    hls: Boolean(w.hls),
    upload: Boolean(w.upload),
    purge: Boolean(w.purge),
    maxBodyBytes: typeof w.max_body_bytes === 'number' ? w.max_body_bytes : null,
    workers: typeof w.workers === 'number' ? w.workers : null,
    ffmpeg: typeof w.ffmpeg === 'boolean' ? w.ffmpeg : null,
    // A Vyzor that did not report rendering is treated as unable to, which is
    // the safe reading: the console then never offers a control it cannot honour.
    render: r
      ? {
          enabled: Boolean(r.enabled),
          available: Boolean(r.available),
          maxConcurrent: typeof r.max_concurrent === 'number' ? r.max_concurrent : 1,
          timeoutSeconds: typeof r.timeout_seconds === 'number' ? r.timeout_seconds : 20,
          viewportWidth: typeof r.viewport_width === 'number' ? r.viewport_width : 1280,
          viewportHeight: typeof r.viewport_height === 'number' ? r.viewport_height : 900,
        }
      : null,
    // And for live video, which is the heaviest of the three.
    rtc: (() => {
      const v = w.rtc;
      if (!v) return null;
      return {
        enabled: Boolean(v.enabled),
        available: Boolean(v.available),
        maxConcurrent: typeof v.max_concurrent === 'number' ? v.max_concurrent : 1,
        active: typeof v.active === 'number' ? v.active : 0,
      };
    })(),
    /*
     * And for the media extractor, with the same safe reading: an older Vyzor
     * does not report it, and the console must not offer a tab whose endpoint is
     * not there.
     */
    extract: (() => {
      const x = w.extract;
      if (!x) return null;
      return { enabled: Boolean(x.enabled), available: Boolean(x.available) };
    })(),
    // The same safe reading for live sessions.
    interactive: (() => {
      const i = w.interactive;
      if (!i) return null;
      return {
        enabled: Boolean(i.enabled),
        available: Boolean(i.available),
        maxConcurrent: typeof i.max_concurrent === 'number' ? i.max_concurrent : 1,
        active: typeof i.active === 'number' ? i.active : 0,
        idleTimeoutSeconds:
          typeof i.idle_timeout_seconds === 'number' ? i.idle_timeout_seconds : 180,
      };
    })(),
  };
}

/**
 * A live-session text message, decoded into the domain.
 *
 * Returns null for a message the console does not understand, so a newer Vyzor
 * can add message kinds without breaking an older console — the parser skips
 * what it cannot place rather than throwing.
 */
import { normalizeLiveMedia } from './live-media';

export function normalizeLiveMessage(w: WireLiveMessage): LiveMessage | null {
  switch (w.type) {
    case 'ready':
      return { kind: 'ready', url: w.url ?? '', width: w.width ?? 0, height: w.height ?? 0 };
    case 'state':
      return {
        kind: 'state',
        page: {
          url: w.page?.url ?? '',
          title: w.page?.title ?? '',
          loading: Boolean(w.page?.loading),
          editing: Boolean(w.page?.editing),
        },
      };
    case 'media':
      return { kind: 'media', items: normalizeLiveMedia(w) };
    case 'closed':
      return { kind: 'closed', reason: w.reason ?? 'the session closed' };
    case 'error':
      return { kind: 'error', message: w.message ?? 'the session reported an error' };
    // A frame header is only meaningful with the binary that follows it, so it
    // is not a message on its own; the socket handler pairs them.
    default:
      return null;
  }
}

export function normalizeEgressSettings(w: WireEgressSettings): EgressSettings {
  return {
    cookieConfigured: Boolean(w.cookies?.configured),
    cookieNames: w.cookies?.names ?? [],
    proxy: w.proxy ?? '',
    impersonate: w.impersonate ?? '',
    identityRetries: w.identity_retries ?? 0,
    captchaProvider: w.captcha_provider ?? '',
    captchaKeySet: Boolean(w.captcha_key_set),
    frontierDbUrl: w.frontier_db_url ?? '',
    frontierDbPath: w.frontier_db_path ?? '',
    frontierTokenSet: Boolean(w.frontier_token_set),
  };
}

export function normalizeStorageConfig(w: WireStorageConfig): StorageConfig {
  return {
    endpoint: w.endpoint ?? null,
    bucket: w.bucket ?? null,
    configured: Boolean(w.configured),
  };
}

export function normalizeSystemStorage(w: WireSystemStorage): SystemStorage {
  const fs = w.filesystem;
  return {
    workDir: w.work_dir ?? '',
    writable: w.writable ?? false,
    writeError: w.write_error ?? null,
    mountPoint: w.mount_point ?? null,
    fsType: w.fs_type ?? null,
    fsDevice: w.fs_device ?? null,
    filesystem: fs
      ? {
          totalBytes: fs.total_bytes ?? 0,
          freeBytes: fs.free_bytes ?? 0,
          availBytes: fs.avail_bytes ?? 0,
          usedBytes: fs.used_bytes ?? 0,
          usedFraction:
            fs.total_bytes > 0 ? Math.min(Math.max(fs.used_bytes / fs.total_bytes, 0), 1) : 0,
        }
      : null,
    library: { bytes: w.library?.bytes ?? 0, files: w.library?.files ?? 0 },
    downloads: {
      completed: w.downloads?.completed ?? 0,
      active: w.downloads?.active ?? 0,
      failed: w.downloads?.failed ?? 0,
      bytesTransferred: w.downloads?.bytes_transferred ?? 0,
    },
  };
}
