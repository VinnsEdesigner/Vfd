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
  Download,
  DownloadState,
  MediaKind,
  Quality,
  ProbeResult,
  Capabilities,
  StorageConfig,
};

/* ------------------------------ status ------------------------------ */

export type StatusTone = 'idle' | 'running' | 'done' | 'failed';

export function statusTone(state: DownloadState): StatusTone {
  if (state === 'running') return 'running';
  if (state === 'done') return 'done';
  if (state === 'failed') return 'failed';
  return 'idle';
}

export function isActiveState(state: DownloadState): boolean {
  return state === 'running' || state === 'queued';
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

/** Bytes-per-second as a transfer rate. `—` when not transferring. */
export function formatRate(bytesPerSecond: number): string {
  if (!bytesPerSecond || bytesPerSecond <= 0) return '—';
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

/* ------------------------------ view models ------------------------------ */

export interface DownloadView {
  id: string;
  url: string;
  name: string;
  state: DownloadState;
  tone: StatusTone;
  /** 0..1 */
  fraction: number;
  percent: number;
  bytes: number;
  bytesLabel: string;
  downloadedLabel: string;
  /** Live transfer rate. Only meaningful while downloading; 0 otherwise. */
  bytesPerSecond: number;
  sha256: string | null;
  artifacts: string[];
  error: string | null;
  createdAtLabel: string;
  kindLabel: string;
  isActive: boolean;
  isSettled: boolean;
  isOpenable: boolean;
  /** What the viewer should render, and which actions make sense. */
  isVideo: boolean;
  isImage: boolean;
  isAudio: boolean;
  qualities: QualityOption[];
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
    name: fileNameFromUrl(d.url),
    state: d.state,
    tone: statusTone(d.state),
    fraction,
    percent: Math.round(fraction * 100),
    bytes: d.bytes,
    bytesLabel: formatBytes(d.bytes),
    downloadedLabel: formatBytes(d.progress.downloadedBytes),
    bytesPerSecond: d.bytesPerSecond,
    sha256: d.sha256,
    artifacts: d.artifacts,
    error: d.error,
    createdAtLabel: formatTimestamp(d.createdAt),
    kindLabel: d.media ? formatKind(d.media.kind) : '—',
    isActive: isActiveState(d.state),
    isSettled: isSettledState(d.state),
    isOpenable: d.state === 'done',
    isVideo: Boolean(d.media?.isVideo),
    isImage: Boolean(d.media?.isImage),
    isAudio: Boolean(d.media?.isAudio),
    qualities: d.media ? toQualityOptions(d.media.qualities) : [],
  };
}

export function toDownloadViews(list: Download[]): DownloadView[] {
  return list.map(toDownloadView);
}
