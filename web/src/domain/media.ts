/**
 * Pure domain models. No imports from any other layer, no framework types.
 *
 * These are the shapes the rest of the app reasons about. Wire formats live in
 * `domain/wire.ts`; the mapping between the two lives in `domain/normalize.ts`.
 */

export type MediaKind =
  | 'mp4'
  | 'matroska'
  | 'avi'
  | 'flv'
  | 'ogg'
  | 'asf'
  | 'mpeg-ts'
  | 'mpeg-ps'
  | 'hls'
  | 'dash'
  // images
  | 'jpeg'
  | 'png'
  | 'gif'
  | 'webp'
  | 'bmp'
  | 'tiff'
  | 'avif'
  | 'heic'
  // audio
  | 'mp3'
  | 'aac'
  | 'flac'
  | 'wav'
  | 'ogg-audio'
  | 'unknown';

/** One selectable quality advertised by a manifest.
 *  `bytes` is -1 when the manifest did not state enough to derive it. */
export interface Quality {
  width: number;
  height: number;
  bandwidth: number;
  bytes: number;
  codecs: string;
  url: string;
}

export interface Media {
  kind: MediaKind;
  container: string;
  confidence: number;
  isVideo: boolean;
  isImage: boolean;
  isAudio: boolean;
  /** Frame size from the container, or 0 when it did not state one. A
   *  single-file source has no rendition list, so this is its only resolution. */
  width: number;
  height: number;
  /** Present for HLS/DASH sources; empty otherwise. */
  qualities: Quality[];
}

export type DownloadState =
  | 'queued'
  | 'running'
  | 'done'
  | 'failed'
  | 'cancelled'
  /** Stopped on purpose; the partial data is kept and it can be resumed. */
  | 'paused';

export interface DownloadProgress {
  downloadedBytes: number;
  totalBytes: number;
}

/**
 * Turning a container the browser cannot play (MKV, AVI) into MP4.
 *
 * `none` means it has not been asked for. A `failed` conversion keeps the
 * original file untouched, so it can be retried or opened with another app.
 */
export type ConversionState = 'none' | 'converting' | 'done' | 'failed';

export interface Conversion {
  state: ConversionState;
  /** Why it failed, when it did. Null otherwise. */
  error: string | null;
}

export interface Download {
  id: string;
  /** What to call this item: the name it was renamed to, or the one derived
   *  from the source URL. Never an id. */
  name: string;
  /**
   * The name as it exists on disk, extension included. What a save-to-device
   * must use: without the extension the OS cannot tell what the file is.
   */
  filename: string;
  url: string;
  state: DownloadState;
  progress: DownloadProgress;
  /** Live transfer rate in bytes/second, 0 when not transferring. */
  bytesPerSecond: number;
  media: Media | null;
  bytes: number;
  sha256: string | null;
  masterPlaylist: string | null;
  /** Container-to-MP4 conversion state, so a viewer can offer it and poll it. */
  conversion: Conversion;
  artifacts: string[];
  error: string | null;
  /** The format the user chose, if any. */
  requestedQuality: string | null;
  connections: number;
  hls: boolean;
  upload: boolean;
  purge: boolean;
  createdAt: number;
  finishedAt: number;
}

export interface ProbeResult {
  url: string;
  finalUrl: string;
  contentType: string | null;
  totalBytes: number;
  acceptRanges: boolean;
  media: Media | null;
}

export interface Capabilities {
  /** The API's own version, e.g. "1". */
  version: string;
  /** The engine build, e.g. "0.1.0". Null when the server predates the field. */
  engineVersion: string | null;
  detectors: MediaKind[];
  hls: boolean;
  upload: boolean;
  purge: boolean;
  /** Request body cap the server enforces, in bytes (null when unreported). */
  maxBodyBytes: number | null;
  /** Connection workers the server runs (null when unreported). */
  workers: number | null;
  /** Whether the host actually has ffmpeg, probed by the engine. */
  ffmpeg: boolean | null;
}

export interface StorageConfig {
  endpoint: string | null;
  bucket: string | null;
  configured: boolean;
}

/** Requests the data layer accepts (already domain-shaped). */
export interface CreateDownloadInput {
  url: string;
  /** Chosen format label, e.g. "1080p" / "Source". */
  quality?: string;
  hls: boolean;
  upload: boolean;
  purge: boolean;
  connections: number;
  /**
   * Accept a payload the detector does not recognise. Off by default: the engine
   * refuses an unexpected payload early, and only a caller that knows it wants a
   * document or a text file should turn that off.
   */
  allowNonMedia?: boolean;
  prefix?: string;
  hlsDir?: string;
  /** Title runtime in seconds. Used only to derive sizes for a manifest
   *  source; 0 means sizes are reported as unknown. */
  durationSeconds?: number;
}

/** Storage credentials the caller may supply per request. */
export interface StorageCredentials {
  endpoint?: string;
  region?: string;
  bucket?: string;
  accessKey?: string;
  secretKey?: string;
  sessionToken?: string;
  cdnBase?: string;
  cfToken?: string;
  cfZone?: string;
  pathStyle?: boolean;
}

export function isTerminal(state: DownloadState): boolean {
  return state === 'done' || state === 'failed' || state === 'cancelled';
}

/** Paused is not terminal: it is waiting to be resumed. */
export function isResumable(state: DownloadState): boolean {
  return state === 'paused';
}

export function progressFraction(p: DownloadProgress): number {
  if (p.totalBytes <= 0) return 0;
  return Math.min(Math.max(p.downloadedBytes / p.totalBytes, 0), 1);
}

export interface FsStats {
  totalBytes: number;
  freeBytes: number;
  availBytes: number;
  usedBytes: number;
  /** 0..1, used / total. */
  usedFraction: number;
}

export interface LibraryUsage {
  bytes: number;
  files: number;
}

export interface DownloadCounts {
  completed: number;
  active: number;
  failed: number;
  bytesTransferred: number;
}

export interface SystemStorage {
  workDir: string;
  /** Whether the engine proved it can write to `workDir`. */
  writable: boolean;
  /** Why not, when `writable` is false. */
  writeError: string | null;
  /** The volume the work dir is on, as the kernel reports it. */
  mountPoint: string | null;
  /** Filesystem type, e.g. "ext4" / "overlay" / "apfs". */
  fsType: string | null;
  /** Backing device, e.g. "/dev/nvme0n1p2" or "overlay". */
  fsDevice: string | null;
  filesystem: FsStats | null;
  library: LibraryUsage;
  downloads: DownloadCounts;
}
