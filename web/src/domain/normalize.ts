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
} from './media';
import { browseKindFor, type BrowseItem, type BrowseResult } from './browse';
import type {
  WireBrowseResult,
  WireSystemStorage,
  WireCapabilities,
  WireDownload,
  WireDownloadList,
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
 * A page listing from the engine, as candidates.
 *
 * The engine has already resolved each reference against the page and kept only
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
            title: filename.replace(/\.[a-z0-9]{1,5}$/i, '') || filename,
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
  return {
    version: w.version ?? '1',
    engineVersion: w.engine_version ?? null,
    detectors: Array.isArray(w.detectors) ? w.detectors.map(toMediaKind) : [],
    hls: Boolean(w.hls),
    upload: Boolean(w.upload),
    purge: Boolean(w.purge),
    maxBodyBytes: typeof w.max_body_bytes === 'number' ? w.max_body_bytes : null,
    workers: typeof w.workers === 'number' ? w.workers : null,
    ffmpeg: typeof w.ffmpeg === 'boolean' ? w.ffmpeg : null,
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
