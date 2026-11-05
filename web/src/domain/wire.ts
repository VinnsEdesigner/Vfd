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
  /** The format the user chose, echoed back by Vyzor. */
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

/** One reference Vyzor found on a page. */
export interface WireBrowseItem {
  url: string;
  filename: string;
  /** The link text, when the reference was an anchor. Null otherwise. */
  title: string | null;
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
  /** Which filter produced this listing. */
  filter?: string;
}

/** One rendition a resolved stream offers. */
export interface WireCrawlQuality {
  height: number;
  label: string;
  /** -1 when the extractor did not say. */
  bytes: number;
  best: boolean;
}

/** One file a site crawl found. */
export interface WireCrawlHit {
  url: string;
  filename: string;
  /** The link text, when the reference was an anchor. */
  title: string | null;
  mime: string;
  /** How well the name matched the term, 0-100. Best match first. */
  score: number;
  /** The page this was found on, so a result can be traced back. */
  found_on: string | null;
  /** True when this page had to be driven through the browser to see it. */
  via_browser: boolean;
  /** True when a media extractor resolved this page, so it is the media itself. */
  resolved: boolean;
  /** The renditions a resolved stream offers, largest first. */
  qualities?: WireCrawlQuality[];
}

export interface WireCrawlResult {
  ok: boolean;
  url: string;
  term: string | null;
  /** How many files matched. */
  found: number;
  /** What the walk did, so an empty result can be explained. */
  pages_read: number;
  pages_failed: number;
  pages_rendered: number;
  skipped_offsite: number;
  items: WireCrawlHit[];
}

export interface WireCrawlRequest {
  url: string;
  term?: string;
  depth?: number;
  max_pages?: number;
  budget_seconds?: number;
  same_host_only?: boolean;
  use_browser?: boolean;
  /**
   * The most pages to drive a browser for.
   *
   * A browser costs several seconds a page, so the crawl bounds the cost by
   * counting rather than by guessing which page needs one.
   */
  max_browser_pages?: number;
  /** CSS selectors to click before reading, for a listing behind an interaction. */
  click_selectors?: string[];
  settle_seconds?: number;
}

/** One rendition of a resolved stream. */
export interface WireExtractFormat {
  url: string;
  format_id: string | null;
  ext: string | null;
  resolution: string | null;
  vcodec: string | null;
  acodec: string | null;
  mime: string | null;
  /** -1 when the extractor did not say. */
  filesize: number;
  bitrate: number;
  height: number;
  has_video: boolean;
  has_audio: boolean;
}

/**
 * One item of a playlist, channel or search.
 *
 * A list is returned without resolving each item, so these carry a title and a
 * page URL but no stream URLs — a chosen one is resolved in a second call.
 */
export interface WireExtractEntry {
  id: string | null;
  title: string | null;
  url: string | null;
  uploader: string | null;
  thumbnail: string | null;
  /** -1 when unknown. */
  duration_seconds: number;
  /** True when this entry is itself a playlist rather than a video. */
  is_playlist: boolean;
}

export interface WireExtractResult {
  ok: boolean;
  url: string | null;
  /** Whether the extractor is installed at all. */
  available: boolean;
  title: string | null;
  /** The site it came from, e.g. "youtube". */
  extractor: string | null;
  /** -1 when unknown. */
  duration_seconds: number;
  thumbnail: string | null;
  format_count: number;
  entry_count: number;
  entries: WireExtractEntry[];
  formats: WireExtractFormat[];
}

export interface WireExtractRequest {
  /** A page to resolve. Omit when searching. */
  url?: string;
  /** A term to search for. Omit when resolving a URL. */
  search?: string;
  /** How many search results. */
  limit?: number;
  /** List a playlist or channel instead of resolving one item. */
  enumerate?: boolean;
  /** Only items from this date, as YYYYMMDD. */
  date_after?: string;
  /** Only items up to this date, as YYYYMMDD. */
  date_before?: string;
  /** Which entries to take, in the extractor's syntax: "1:5", "1,3,5", "-5:". */
  playlist_items?: string;
  /** A filter the extractor applies while listing: `duration > 600`. */
  match_filter?: string;
  /** Per-site arguments, `IE_KEY:ARGS`. */
  extractor_args?: string[];
  timeout_seconds?: number;
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

/** Whether Vyzor can render pages, and within what bounds. */
export interface WireRenderCapability {
  enabled: boolean;
  /** Enabled *and* a browser was found on the host. */
  available: boolean;
  max_concurrent: number;
  timeout_seconds: number;
  viewport_width: number;
  viewport_height: number;
}

export interface WireCapabilities {
  version: string;
  detectors: string[];
  hls: boolean;
  upload: boolean;
  purge: boolean;
  max_body_bytes?: number;
  workers?: number;
  vyzor_version?: string;
  ffmpeg?: boolean;
  /** Absent on Vyzors that predate rendering. */
  render?: WireRenderCapability;
  /** Absent on Vyzors that predate live sessions. */
  interactive?: WireInteractiveCapability;
  /** Absent on Vyzors that predate live video. */
  rtc?: WireRtcCapability;
  /** Absent on Vyzors that predate the media extractor. */
  extract?: WireExtractCapability;
}

/** Whether the media extractor is installed on that Vyzor. */
export interface WireExtractCapability {
  enabled: boolean;
  available: boolean;
}

/** Whether a page can be driven live, and within what bounds. */
export interface WireInteractiveCapability {
  enabled: boolean;
  /** Enabled *and* a browser was found on the host. */
  available: boolean;
  max_concurrent: number;
  active: number;
  /** Absent on a Vyzor that predates the field; 0 means "no idle timeout". */
  idle_timeout_seconds?: number;
}

/*
 * Live-session WebSocket messages.
 *
 * Vyzor speaks two kinds: a JSON text message, and a binary image frame
 * introduced by a JSON header. Both are described here so the console's parser
 * has one place to look.
 */

/** The header sent immediately before a binary image frame. */
export interface WireLiveImageHeader {
  type: 'frame' | 'shot';
  format: string;
  width: number;
  height: number;
  seq: number;
}

/** The page's own state, as the browser reports it. */
export interface WireLivePage {
  url: string;
  title: string;
  loading: boolean;
  /**
   * Whether the page has an editable element focused.
   *
   * Vyzor reports this because the console cannot see the page's DOM. It is
   * what decides whether a tap should raise the on-screen keyboard — a tap on a
   * link must not.
   */
  editing?: boolean;
}

/** A live-session text message from Vyzor. */
/** One quality behind a manifest, as Vyzor reports it. */
export interface WireLiveMediaVariant {
  label?: string;
  width?: number;
  height?: number;
  bandwidth?: number;
  url?: string;
}

/**
 * One thing the page loaded that can be downloaded.
 *
 * `bytes` is what the browser actually received, and is -1 when the response
 * had no length — normal for a live stream.
 */
export interface WireLiveMedia {
  url?: string;
  kind?: string;
  mime?: string;
  bytes?: number;
  manifest?: boolean;
  variants?: WireLiveMediaVariant[];
  /** The viewer clicked or long-pressed this one. */
  picked?: boolean;
  /** What the page called it — an alt text, a link title, a caption. */
  title?: string;
  /** A small image the page showed for it. */
  thumb?: string;
  width?: number;
  height?: number;
}

export interface WireLiveMessage {
  type: 'ready' | 'state' | 'closed' | 'error' | 'frame' | 'shot' | 'media';
  url?: string;
  width?: number;
  height?: number;
  reason?: string;
  message?: string;
  page?: WireLivePage;
  format?: string;
  seq?: number;
  /** The list, on a `media` message. */
  items?: WireLiveMedia[];
}

/**
 * Vyzor's answer to `POST /api/v1/rtc`: the session token and the offer.
 *
 * There is no media here — this is the signalling, and the media arrives on the
 * RTCPeerConnection the console builds from `sdp`.
 */
export interface WireRtcOffer {
  token?: string;
  sdp?: string;
  width?: number;
  height?: number;
  /** Whether Vyzor finished gathering, so the offer is final. */
  gathered?: boolean;
}

/** Vyzor's answer to `GET /api/v1/rtc/{token}`. */
export interface WireRtcStatus {
  state?: string;
  connected?: boolean;
  reason?: string;
}

/** Vyzor's live-video capability block. */
export interface WireRtcCapability {
  enabled?: boolean;
  available?: boolean;
  max_concurrent?: number;
  active?: number;
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
