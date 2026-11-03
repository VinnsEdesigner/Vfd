/**
 * Wire types: exactly what the C server sends and receives.
 *
 * Kept separate from the domain so a server change is absorbed by the
 * normalizers in one place instead of rippling through the app. Every field is
 * optional where the server may omit it, because C's JSON writer emits `null`
 * for unset strings.
 */

export interface WireQuality {
  width: number;
  height: number;
  bandwidth: number;
  bytes: number;
  codecs: string;
  url: string;
}

export interface WireMedia {
  kind: string;
  container: string;
  confidence: number;
  is_video: boolean;
  is_image?: boolean;
  is_audio?: boolean;
  /** Frame size, when the container states it. 0 when unknown. */
  width?: number;
  height?: number;
  qualities?: WireQuality[];
}

export interface WireDownloadProgress {
  downloaded_bytes: number;
  total_bytes: number;
  /** Optional: absent on servers that do not report a live rate. */
  bytes_per_second?: number;
}

/**
 * How a container the browser cannot play is being turned into MP4.
 *
 *   none       — not asked for
 *   converting — ffmpeg is running now
 *   done       — the MP4 is on disk and served at /converted
 *   failed     — it could not be converted; `error` says why
 */
export interface WireConversion {
  state: string;
  error: string | null;
}

export interface WireDownload {
  id: string;
  /** Display name. Optional: older servers only sent the id and url. */
  name?: string;
  /**
   * The name as it exists on disk, extension included. Optional: servers before
   * this field only sent `name`, which may have no extension.
   */
  filename?: string;
  url: string;
  state: string;
  progress: WireDownloadProgress;
  media: WireMedia | null;
  bytes: number;
  sha256: string | null;
  master_playlist: string | null;
  /** Absent on servers that predate container conversion. */
  conversion?: WireConversion;
  artifacts: string[];
  error: string | null;
  /** The format the user chose, echoed back by the engine. */
  requested_quality?: string | null;
  connections?: number;
  hls?: boolean;
  upload?: boolean;
  purge?: boolean;
  created_at: number;
  finished_at: number;
}

export interface WireDownloadList {
  items: WireDownload[];
  count: number;
}

export interface WireProbeRequest {
  url: string;
  duration_seconds?: number;
}

/** One media reference the engine found on a page. */
export interface WireBrowseItem {
  url: string;
  filename: string;
  mime: string;
  /** -1 when the source did not say. */
  bytes: number;
}

export interface WireBrowseResult {
  url: string;
  /** The page's own title, when it had one. */
  title: string | null;
  items: WireBrowseItem[];
  count: number;
}

export interface WireDeleteResult {
  deleted?: boolean;
  cancelled?: boolean;
  files_removed?: boolean;
}

export interface WireProbeResult {
  url: string;
  final_url: string | null;
  content_type: string | null;
  total_bytes: number;
  accept_ranges: boolean;
  media: WireMedia | null;
}

export interface WireCapabilities {
  version: string;
  detectors: string[];
  hls: boolean;
  upload: boolean;
  purge: boolean;
  max_body_bytes?: number;
  workers?: number;
  engine_version?: string;
  ffmpeg?: boolean;
}

export interface WireStorageConfig {
  endpoint: string | null;
  bucket: string | null;
  configured: boolean;
}

export interface WireRenameRequest {
  name: string;
}

export interface WireMoveRequest {
  move_to: string;
}

export interface WireActionRequest {
  action: 'pause' | 'resume' | 'convert';
}

export interface WireCreateDownloadRequest {
  url: string;
  /** Chosen format label, e.g. "1080p". Recorded for display. */
  quality?: string;
  hls?: boolean;
  upload?: boolean;
  purge?: boolean;
  connections?: number;
  /** Accept a payload the detector does not recognise (a PDF, a text file). */
  allow_non_media?: boolean;
  prefix?: string;
  hls_dir?: string;
}

export interface WireError {
  error: string;
  status: number;
}

export interface WireFsStats {
  total_bytes: number;
  free_bytes: number;
  avail_bytes: number;
  used_bytes: number;
}

export interface WireSystemStorage {
  work_dir: string;
  writable?: boolean;
  write_error?: string | null;
  mount_point?: string | null;
  fs_type?: string | null;
  fs_device?: string | null;
  filesystem: WireFsStats | null;
  library: { bytes: number; files: number };
  downloads: { completed: number; active: number; failed: number; bytes_transferred: number };
}

export interface WireSetLocationResult {
  dir: string;
  writable: boolean;
}
