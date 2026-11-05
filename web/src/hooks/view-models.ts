/**
 * View models and formatters for the UI.
 *
 * The boundary rule forbids `ui` from importing `domain`, so this module is the
 * bridge: it re-shapes domain objects into exactly what a component renders,
 * and owns the formatting (bytes, rates, durations) that is a presentation
 * concern rather than a domain one.
 *
 * UI imports these from `../hooks`, never from `../domain`.
 */

import type {
  Conversion,
  Download,
  DownloadState,
  MediaKind,
  Quality,
  ProbeResult,
  Capabilities,
  StorageConfig,
} from '../domain/media';
import { isTerminal } from '../domain/media';

/** Re-exported so components can type their props without reaching into domain. */
export type {
  Conversion,
  Download,
  DownloadState,
  MediaKind,
  Quality,
  ProbeResult,
  Capabilities,
  StorageConfig,
};

/* ------------------------------ status ------------------------------ */

export type StatusTone = 'idle' | 'running' | 'done' | 'failed' | 'paused';

export function statusTone(state: DownloadState): StatusTone {
  if (state === 'running') return 'running';
  if (state === 'done') return 'done';
  if (state === 'failed') return 'failed';
  if (state === 'paused') return 'paused';
  return 'idle';
}

export function isActiveState(state: DownloadState): boolean {
  return state === 'running' || state === 'queued';
}

/** Paused: deliberately stopped, waiting to be resumed. Not active (nothing is
 *  transferring) and not settled (it is not finished). */
export function isResumableState(state: DownloadState): boolean {
  return state === 'paused';
}

export function isSettledState(state: DownloadState): boolean {
  return isTerminal(state);
}

/** Human label for a state (the domain value, title-cased where useful). */
export function statusLabel(state: DownloadState): string {
  return state;
}

/* ------------------------------ formatting ------------------------------ */

const BYTE_UNITS = ['B', 'KB', 'MB', 'GB', 'TB'] as const;

export function formatBytes(bytes: number): string {
  if (!bytes || bytes < 0) return '0 B';
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < BYTE_UNITS.length - 1) {
    value /= 1024;
    unit += 1;
  }
  const digits = value < 10 && unit > 0 ? 1 : 0;
  return `${value.toFixed(digits)} ${BYTE_UNITS[unit]}`;
}

/**
 * Bytes-per-second as a transfer rate. `—` when not transferring.
 *
 * Scales the unit, because a slow transfer rendered as "0.0 MB/s" tells the user
 * nothing — which is exactly when they most want to know the speed.
 */
export function formatRate(bytesPerSecond: number): string {
  if (!bytesPerSecond || bytesPerSecond <= 0) return '—';
  if (bytesPerSecond < 1000) return `${Math.round(bytesPerSecond)} B/s`;
  if (bytesPerSecond < 1e6) return `${(bytesPerSecond / 1000).toFixed(1)} KB/s`;
  return `${(bytesPerSecond / 1e6).toFixed(1)} MB/s`;
}

/** Seconds as `m:ss`. */
export function formatDuration(seconds: number): string {
  const safe = Math.max(0, Math.floor(seconds));
  const minutes = Math.floor(safe / 60);
  const secs = safe % 60;
  return `${minutes}:${String(secs).padStart(2, '0')}`;
}

/** Unix seconds as a short local timestamp. */
export function formatTimestamp(unixSeconds: number): string {
  if (!unixSeconds) return '—';
  return new Date(unixSeconds * 1000).toLocaleString('en-GB', {
    day: '2-digit',
    month: 'short',
    hour: '2-digit',
    minute: '2-digit',
  });
}

/** The host of a source URL, for a readable "where did this come from" line.
 *  Falls back to the whole string when it does not parse as a URL. */
export function hostFromUrl(url: string): string {
  try {
    const parsed = new URL(url);
    return parsed.host || url;
  } catch {
    return url;
  }
}

/** The path (no host, no query) of a source URL, for a secondary detail line. */
export function pathFromUrl(url: string): string {
  try {
    const parsed = new URL(url);
    return parsed.pathname;
  } catch {
    return '';
  }
}

/** Lowercase extension of a filename, or '' when it has none. */
export function extensionOf(name: string): string {
  const dot = name.lastIndexOf('.');
  if (dot <= 0 || dot === name.length - 1) return '';
  return name.slice(dot + 1).toLowerCase();
}

/** The extension the OS should see for a detected media kind. */
const EXT_BY_KIND: Record<string, string> = {
  mp4: 'mp4',
  matroska: 'mkv',
  avi: 'avi',
  flv: 'flv',
  ogg: 'ogv',
  asf: 'wmv',
  'mpeg-ts': 'ts',
  'mpeg-ps': 'mpg',
  hls: 'm3u8',
  dash: 'mpd',
  jpeg: 'jpg',
  png: 'png',
  gif: 'gif',
  webp: 'webp',
  bmp: 'bmp',
  tiff: 'tiff',
  avif: 'avif',
  heic: 'heic',
  mp3: 'mp3',
  aac: 'aac',
  flac: 'flac',
  wav: 'wav',
  'ogg-audio': 'ogg',
};

/**
 * A filename an OS can act on: the on-disk name when Vyzor reported one,
 * otherwise the display name with the detected kind's extension appended. A
 * save with no extension produces a file no device knows how to open.
 */
export function filenameFor(d: Download): string {
  const given = d.filename && d.filename.trim() ? d.filename : d.name;
  const base = given && given.trim() ? given : fileNameFromUrl(d.url);
  if (extensionOf(base)) return base;
  const ext = d.media ? EXT_BY_KIND[d.media.kind] : undefined;
  return ext ? `${base}.${ext}` : base;
}

/** Filename from a URL, falling back to the last path segment. */
export function fileNameFromUrl(url: string): string {
  const withoutQuery = url.split('?')[0]?.split('#')[0] ?? url;
  const segment = withoutQuery.split('/').filter(Boolean).pop();
  return segment && segment.length > 0 ? segment : 'download';
}

export function formatKind(kind: MediaKind): string {
  switch (kind) {
    case 'mp4': return 'MP4';
    case 'matroska': return 'MKV';
    case 'mpeg-ts': return 'MPEG-TS';
    case 'hls': return 'HLS';
    case 'dash': return 'DASH';
    case 'jpeg': return 'JPEG';
    case 'png': return 'PNG';
    case 'gif': return 'GIF';
    case 'webp': return 'WebP';
    case 'bmp': return 'BMP';
    case 'tiff': return 'TIFF';
    case 'avif': return 'AVIF';
    case 'heic': return 'HEIC';
    case 'mp3': return 'MP3';
    case 'aac': return 'AAC';
    case 'flac': return 'FLAC';
    case 'wav': return 'WAV';
    case 'ogg-audio': return 'Ogg';
    case 'unknown': return '—';
    default: return kind.toUpperCase();
  }
}

/** A quality label like "1080p" / "4K" / "8K". */
export function qualityLabel(height: number): string {
  if (height <= 0) return '—';
  if (height >= 4320) return '8K';
  if (height >= 2160) return '4K';
  if (height >= 1440) return '2K';
  return `${height}p`;
}

/** Standard rungs the picker offers, largest first. */
export const QUALITY_STEPS = [4320, 2160, 1440, 1080, 720, 480, 360, 240] as const;

export interface QualityOption {
  height: number;
  width: number;
  label: string;
  /** -1 when the manifest did not state enough to derive a size. */
  bytes: number;
  sizeLabel: string;
  codecs: string;
  url: string;
}

export function toQualityOptions(qualities: readonly Quality[]): QualityOption[] {
  return qualities
    .map((q) => ({
      height: q.height,
      width: q.width,
      label: qualityLabel(q.height),
      bytes: q.bytes,
      // Never invent a size: an unknown one says so.
      sizeLabel: q.bytes >= 0 ? formatBytes(q.bytes) : 'size unknown',
      codecs: q.codecs,
      url: q.url,
    }))
    .sort((a, b) => b.height - a.height);
}

/* ------------------------------ pipeline ------------------------------ */

export type PipelineStageKey = 'probe' | 'fetch' | 'verify' | 'transcode' | 'upload' | 'purge';

export type PipelineStageStatus = 'done' | 'active' | 'pending' | 'skipped' | 'fault';

export interface PipelineStageView {
  key: PipelineStageKey;
  status: PipelineStageStatus;
}

/** Which optional stages a given download was asked to run. */
export interface PipelineOptions {
  hls: boolean;
  upload: boolean;
  purge: boolean;
}

/**
 * Where a download has actually got to, from observable facts only:
 *   - a record exists, so the probe succeeded;
 *   - `bytes > 0` means the fetch finished;
 *   - `sha256` is written after verification;
 *   - `masterPlaylist` / `artifacts` are the output of transcode / upload.
 *
 * A stage Vyzor cannot report on stays "pending" rather than being guessed
 * at, because a pipeline that shows a stage as done when it is not is worse
 * than one that admits it does not know. Stages a job was not asked to run are
 * "skipped", not "done".
 */
export function pipelineStages(
  d: DownloadView,
  options: PipelineOptions,
): PipelineStageView[] {
  // A paused download stopped at the fetch stage and is waiting, not failed.
  if (d.state === 'paused') {
    return [
      { key: 'probe', status: 'done' },
      { key: 'fetch', status: 'active' },
      { key: 'verify', status: 'pending' },
      { key: 'transcode', status: options.hls ? 'pending' : 'skipped' },
      { key: 'upload', status: options.upload ? 'pending' : 'skipped' },
      { key: 'purge', status: options.purge && options.upload ? 'pending' : 'skipped' },
    ];
  }
  const finished = d.state === 'done';
  const faulted = d.state === 'failed';
  const running = d.state === 'running' || d.state === 'queued';
  const fetched = d.bytes > 0;

  const stages: PipelineStageView[] = [
    { key: 'probe', status: 'done' },
    { key: 'fetch', status: fetched ? 'done' : faulted ? 'fault' : running ? 'active' : 'pending' },
    {
      key: 'verify',
      status: d.sha256 ? 'done' : faulted ? 'fault' : finished ? 'done' : 'pending',
    },
  ];

  const optional = (
    key: PipelineStageKey,
    wanted: boolean,
    evidence: boolean,
  ): PipelineStageView => {
    if (!wanted) return { key, status: 'skipped' };
    if (evidence) return { key, status: 'done' };
    if (faulted && fetched) return { key, status: 'fault' };
    if (finished) return { key, status: 'done' };
    return { key, status: 'pending' };
  };

  stages.push(optional('transcode', options.hls, d.masterPlaylist !== null));
  stages.push(optional('upload', options.upload, d.artifacts.length > 0));
  stages.push(optional('purge', options.purge && options.upload, false));
  return stages;
}

/* ------------------------------ view models ------------------------------ */

export interface DownloadView {
  id: string;
  url: string;
  name: string;
  state: DownloadState;
  tone: StatusTone;
  /**
   * Where the bytes live: Vyzor's library, or this device. A device copy is
   * a first-class entry in the file manager (playable, shareable, openable), not
   * only a note on a Vyzor record.
   */
  source: 'engine' | 'device' | 'remote';
  /** The playback URL for a device copy; null for a Vyzor item. */
  localUrl: string | null;
  /** 0..1 */
  fraction: number;
  percent: number;
  bytes: number;
  bytesLabel: string;
  downloadedLabel: string;
  /** Live transfer rate. Only meaningful while downloading; 0 otherwise. */
  bytesPerSecond: number;
  sha256: string | null;
  /** Master playlist path, present once HLS transcode produced output. */
  masterPlaylist: string | null;
  /** Container-to-MP4 conversion state, for the viewer's convert action. */
  conversion: Conversion;
  artifacts: string[];
  error: string | null;
  createdAtLabel: string;
  /** Raw timestamp, so sorting is numeric rather than string-comparing a
   *  formatted date. */
  createdAt: number;
  kindLabel: string;
  /** The filename a save should use, extension included. */
  filename: string;
  /** Frame size from the container, e.g. "640×360". Null when not stated. */
  resolutionLabel: string | null;
  /** Lowercase file extension from the name, e.g. "mp4" / "png". Used to decide
   *  whether the grid can show a real thumbnail. */
  extension: string;
  isActive: boolean;
  /** Paused: can be resumed from where it stopped. */
  isResumable: boolean;
  isSettled: boolean;
  isOpenable: boolean;
  /** What the viewer should render, and which actions make sense. */
  isVideo: boolean;
  isImage: boolean;
  isAudio: boolean;
  qualities: QualityOption[];
  /** Which stages this download ran, and where each got to. */
  pipeline: PipelineStageView[];
  /** The format the user chose, and the options the job runs — recorded by the
   *  Vyzor, so "download info" reports what actually happened. */
  requestedQuality: string | null;
  connections: number;
  hls: boolean;
  upload: boolean;
  purge: boolean;
}

export function toDownloadView(d: Download): DownloadView {
  const fraction =
    d.progress.totalBytes > 0
      ? Math.min(Math.max(d.progress.downloadedBytes / d.progress.totalBytes, 0), 1)
      : d.bytes > 0
        ? Math.min(Math.max(d.progress.downloadedBytes / d.bytes, 0), 1)
        : 0;

  return {
    id: d.id,
    url: d.url,
    // The server now names an item, and a rename changes it. Fall back to the
    // URL segment so a record from an older server still reads sensibly.
    name: d.name && d.name.trim() ? d.name : fileNameFromUrl(d.url),
    source: 'engine',
    localUrl: null,
    state: d.state,
    tone: statusTone(d.state),
    fraction,
    percent: Math.round(fraction * 100),
    bytes: d.bytes,
    bytesLabel: formatBytes(d.bytes),
    downloadedLabel: formatBytes(d.progress.downloadedBytes),
    bytesPerSecond: d.bytesPerSecond,
    sha256: d.sha256,
    masterPlaylist: d.masterPlaylist,
    conversion: d.conversion,
    artifacts: d.artifacts,
    error: d.error,
    createdAtLabel: formatTimestamp(d.createdAt),
    createdAt: d.createdAt,
    kindLabel: d.media ? formatKind(d.media.kind) : '—',
    filename: filenameFor(d),
    resolutionLabel:
      d.media && d.media.width > 0 && d.media.height > 0
        ? `${d.media.width}\u00d7${d.media.height}`
        : null,
    extension: extensionOf(d.name),
    isActive: isActiveState(d.state),
    isResumable: isResumableState(d.state),
    isSettled: isSettledState(d.state),
    isOpenable: d.state === 'done',
    isVideo: Boolean(d.media?.isVideo),
    isImage: Boolean(d.media?.isImage),
    isAudio: Boolean(d.media?.isAudio),
    qualities: d.media ? toQualityOptions(d.media.qualities) : [],
    pipeline: [],
    requestedQuality: d.requestedQuality,
    connections: d.connections,
    hls: d.hls,
    upload: d.upload,
    purge: d.purge,
  };
}

/** Fills in the pipeline for a view once its options are known. */
export function withPipeline(
  view: DownloadView,
  options: PipelineOptions,
): DownloadView {
  return { ...view, pipeline: pipelineStages(view, options) };
}

export function toDownloadViews(list: Download[]): DownloadView[] {
  return list.map(toDownloadView);
}

/**
 * A browse result as something the viewer can render.
 *
 * Previewing a remote candidate must not require downloading it first, so the
 * viewer needs a view model for a file that is not in the library and may never
 * be. `source: 'remote'` keeps it honest — nothing has been saved.
 */
export function toRemoteView(item: {
  id: string;
  title: string;
  filename: string;
  url: string;
  mime: string;
  bytes: number;
  thumbUrl: string | null;
  licence: string;
}): DownloadView {
  const kind = kindFromMime(item.mime, item.filename);
  const ext = extensionOf(item.filename);
  return {
    id: item.id,
    url: item.url,
    name: item.filename,
    source: 'remote',
    localUrl: item.thumbUrl,
    state: 'done',
    tone: 'done',
    fraction: 1,
    percent: 100,
    bytes: item.bytes,
    bytesLabel: item.bytes >= 0 ? formatBytes(item.bytes) : 'unknown size',
    downloadedLabel: '',
    bytesPerSecond: 0,
    sha256: null,
    masterPlaylist: null,
    // A remote candidate is never converted: it is not in the library.
    conversion: { state: 'none', error: null },
    artifacts: [],
    error: null,
    createdAtLabel: '—',
    createdAt: 0,
    kindLabel: formatKind(kind),
    filename: item.filename,
    resolutionLabel: null,
    extension: ext,
    isActive: false,
    isResumable: false,
    isSettled: true,
    isOpenable: true,
    isVideo: VIDEO_KINDS.has(kind),
    isImage: IMAGE_KINDS.has(kind),
    isAudio: AUDIO_KINDS.has(kind),
    qualities: [],
    pipeline: [],
    requestedQuality: null,
    connections: 0,
    hls: false,
    upload: false,
    purge: false,
  };
}

/** A stored file on this device, as the file manager needs to see it. */
export interface DeviceFileInfo {
  id: string;
  name: string;
  type: string;
  size: number;
  /** A URL the browser can play. */
  url: string;
}

/**
 * A device-stored file as a first-class library entry.
 *
 * It carries everything the file manager acts on — kind, extension, a playable
 * URL — so a device file is played in place instead of being handed to a new
 * tab. Vyzor knows nothing about these; they are the user's own copies.
 */
export function toDeviceView(f: DeviceFileInfo): DownloadView {
  const kind = kindFromMime(f.type, f.name);
  const ext = extensionOf(f.name);
  const isVideo = VIDEO_KINDS.has(kind);
  const isImage = IMAGE_KINDS.has(kind);
  const isAudio = AUDIO_KINDS.has(kind);
  return {
    id: f.id,
    // No source URL: the bytes are here, not on a server.
    url: '',
    name: f.name,
    source: 'device',
    localUrl: f.url,
    // A stored file is complete by definition — there is nothing left to fetch.
    state: 'done',
    tone: 'done',
    fraction: 1,
    percent: 100,
    bytes: f.size,
    bytesLabel: formatBytes(f.size),
    downloadedLabel: formatBytes(f.size),
    bytesPerSecond: 0,
    sha256: null,
    masterPlaylist: null,
    // The bytes are already here; Vyzor has nothing to convert.
    conversion: { state: 'none', error: null },
    artifacts: [],
    error: null,
    createdAtLabel: '\u2014',
    createdAt: 0,
    kindLabel: formatKind(kind),
    filename: f.name,
    resolutionLabel: null,
    extension: ext,
    isActive: false,
    isResumable: false,
    isSettled: true,
    isOpenable: true,
    isVideo,
    isImage,
    isAudio,
    qualities: [],
    pipeline: [],
    requestedQuality: null,
    connections: 0,
    hls: false,
    upload: false,
    purge: false,
  };
}

const VIDEO_KINDS = new Set<MediaKind>(['mp4', 'matroska', 'avi', 'flv', 'ogg', 'asf', 'mpeg-ts', 'mpeg-ps', 'hls', 'dash']);
const IMAGE_KINDS = new Set<MediaKind>(['jpeg', 'png', 'gif', 'webp', 'bmp', 'tiff', 'avif', 'heic']);
const AUDIO_KINDS = new Set<MediaKind>(['mp3', 'aac', 'flac', 'wav', 'ogg-audio']);

/** The container a device file holds, from its MIME type and, failing that, its
 *  extension. A file the OS could not type is not assumed to be anything. */
export function kindFromMime(mime: string, name: string): MediaKind {
  if (mime.startsWith('video/')) {
    const sub = mime.slice('video/'.length).split(';')[0] ?? '';
    if (sub === 'webm') return 'matroska';
    if (sub === 'x-msvideo') return 'avi';
    if (sub === 'x-matroska') return 'matroska';
    if (sub === 'quicktime') return 'mp4';
    return 'mp4';
  }
  if (mime.startsWith('audio/')) {
    const sub = mime.slice('audio/'.length).split(';')[0] ?? '';
    if (sub === 'wav' || sub === 'x-wav') return 'wav';
    if (sub === 'flac' || sub === 'x-flac') return 'flac';
    if (sub === 'aac') return 'aac';
    if (sub === 'ogg') return 'ogg-audio';
    return 'mp3';
  }
  if (mime.startsWith('image/')) {
    const sub = mime.slice('image/'.length).split(';')[0] ?? '';
    if (sub === 'png' || sub === 'gif' || sub === 'webp' || sub === 'bmp' || sub === 'avif') {
      return sub as MediaKind;
    }
    if (sub === 'tiff') return 'tiff';
    return 'jpeg';
  }
  // A file whose type the OS did not know still has an extension worth reading.
  const ext = extensionOf(name);
  if (['mp4', 'm4v', 'mov', 'webm', 'mkv', 'avi', 'flv', 'ts', 'mpg'].includes(ext)) return 'mp4';
  if (['mp3', 'm4a', 'wav', 'flac', 'aac', 'ogg', 'oga'].includes(ext)) return 'mp3';
  if (['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'tiff', 'avif', 'heic'].includes(ext)) {
    return 'jpeg';
  }
  return 'unknown';
}
