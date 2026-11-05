/**
 * What an in-app search can find.
 *
 * These are *candidates* — files that live somewhere else on the internet and can
 * be fetched. They are not library items: nothing here is on this device or in
 * Vyzor until the user asks for it. Keeping the two apart is why the browse
 * view cannot accidentally present a remote URL as if it were already saved.
 *
 * Pure domain: no framework types, no fetch, no React.
 */

/** What kind of thing a result is, as far as we can tell before downloading it. */
export type BrowseKind = 'image' | 'audio' | 'video' | 'document' | 'text' | 'other';

export interface BrowseItem {
  /** Stable id, unique within a source. */
  id: string;
  /** Human title, without the file extension where the source gives one. */
  title: string;
  /** Which provider this came from, for attribution. */
  source: string;
  /** The page a person would visit to learn about it, when there is one. */
  pageUrl: string | null;
  kind: BrowseKind;
  /** Content type as reported by the source; may be empty. */
  mime: string;
  /** Filename to save under, extension included. */
  filename: string;
  /** Bytes, or -1 when the source did not say. */
  bytes: number;
  /** The bytes themselves. */
  url: string;
  /** A smaller version for the grid, when the source offers one. */
  thumbUrl: string | null;
  /** Human-readable licence, e.g. "CC BY-SA 4.0". Empty when unknown. */
  licence: string;
  /** Frame size for images and video, when known. */
  width: number | null;
  height: number | null;
  /** Duration in seconds for audio and video, when known. */
  durationSeconds: number | null;
}

export interface BrowseQuery {
  /** What the user typed. */
  term: string;
  /** Restrict to one kind, or 'all'. */
  kind: BrowseKind | 'all';
  /** 1-based. */
  page: number;
  /** How many per page. */
  perPage: number;
}

/**
 * How much of a page to list.
 *
 *   media  only what this app can play or show — the default.
 *   all    every reference, the way a browser sees the page: links to other
 *          pages, stylesheets, archives. This is reading a site, not hunting
 *          for a file to play.
 */
export type BrowseFilter = 'media' | 'all';

/**
 * A page Vyzor fetched, to be shown rather than listed.
 *
 * `framable` is whether the site permits being embedded. A browser enforces the
 * site's own X-Frame-Options / CSP on the framed document, so a refusal is
 * reported up front rather than left as a frame that silently stays blank.
 *
 * This is the *fetch* path. A site that refuses framing — or that builds itself
 * with JavaScript — cannot be shown this way, which is why there is also the
 * renderer: it runs a real browser and returns a picture instead.
 */
export interface FetchedPage {
  /** The URL Vyzor should be asked to serve the bytes from. */
  url: string;
  framable: boolean;
  /** Which header refused framing, when one did. */
  blockedBy: string | null;
}

/**
 * What the renderer produced for a page.
 *
 *   image  a picture of the page, which is the mode that cannot be got subtly
 *          wrong: a PNG has no scripts to sanitise.
 *   html   the rendered DOM, after its scripts have run.
 */
export type RenderMode = 'image' | 'html';

/** A rendered page, or the reason there is none. */
export interface RenderedPage {
  /** Where the console points an <img> or a frame. */
  url: string;
  mode: RenderMode;
}

/**
 * Why a render did not happen. Kept as a small set so the console can say
 * something specific for each rather than one vague failure.
 */
export type RenderFailure =
  | 'disabled'   /* Vyzor has rendering turned off */
  | 'unavailable'/* enabled, but no browser on the host */
  | 'busy'       /* the gate is full; worth retrying */
  | 'timeout'    /* the page did not settle */
  | 'refused'    /* the URL is not one we render */
  | 'failed';    /* anything else */

/*
 * A live, driveable page.
 *
 * The renderer (above) draws a page once; this keeps a browser alive so the page
 * can be *used* — typed in, scrolled, followed. It is a WebSocket rather than a
 * request because it is a stream in both directions: frames out, input in.
 *
 * The capability that says whether this is possible lives in `media.ts` with the
 * rest of Vyzor capabilities; the wire shape is in `wire.ts`.
 */

/** Why a live session ended, or could not start. */
export type LiveFailure =
  | 'disabled'    /* Vyzor has live sessions turned off */
  | 'unavailable' /* enabled, but no browser on the host */
  | 'busy'        /* the gate is full; worth retrying */
  | 'refused'     /* the URL is not one we drive */
  | 'closed'      /* the session ended while in use */
  | 'failed';     /* anything else */

/** The page's own state, as the browser reports it. */
export interface LivePageState {
  url: string;
  title: string;
  loading: boolean;
  /**
   * Whether the page has an editable element focused.
   *
   * The console raises its on-screen keyboard only when this is true, so a tap
   * on a link behaves like a tap on a link rather than opening a keyboard.
   */
  editing: boolean;
}

/** A single decoded image from a live session. */
export interface LiveFrame {
  /** The bytes, as a blob URL the canvas or an <img> can show. */
  url: string;
  /** A full-resolution still from a screenshot request, rather than a stream frame. */
  still: boolean;
  width: number;
  height: number;
  seq: number;
}

/**
 * A live-session message the console acts on.
 *
 * Image frames are not here: they are binary and arrive with a header, so the
 * socket handler pairs them into a `LiveFrame` rather than routing them through
 * this.
 */
import type {
  LiveMediaItem,
  LiveMediaKind,
  LiveMediaVariant,
} from './live-media';

export type { LiveMediaItem, LiveMediaKind, LiveMediaVariant };

export type LiveMessage =
  | { kind: 'ready'; url: string; width: number; height: number }
  | { kind: 'state'; page: LivePageState }
  | { kind: 'media'; items: LiveMediaItem[] }
  | { kind: 'closed'; reason: string }
  | { kind: 'error'; message: string };

export interface BrowseResult {
  items: BrowseItem[];
  /** Total the source reports, when it says. -1 when unknown. */
  total: number;
  /** True when another page is likely to exist. */
  hasMore: boolean;
  /**
   * The listing page's own title, when it had one. Only a URL listing has this;
   * a search has no single page behind it.
   */
  title?: string | null;
}

/** Which kinds a given MIME type and filename imply. */
export function browseKindFor(mime: string, filename: string): BrowseKind {
  const m = mime.toLowerCase().split(';')[0]?.trim() ?? '';
  if (m.startsWith('image/')) return 'image';
  if (m.startsWith('audio/')) return 'audio';
  if (m.startsWith('video/')) return 'video';
  if (m === 'application/pdf') return 'document';
  if (m.startsWith('text/')) return 'text';
  // Sources often omit the type, so fall back to the extension.
  const ext = filename.toLowerCase().split('.').pop() ?? '';
  if (['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'tiff', 'avif', 'svg'].includes(ext)) return 'image';
  if (['mp3', 'wav', 'flac', 'aac', 'oga', 'ogg', 'm4a'].includes(ext)) return 'audio';
  if (['mp4', 'webm', 'mkv', 'avi', 'mov', 'ogv', 'm4v'].includes(ext)) return 'video';
  if (['pdf', 'djvu'].includes(ext)) return 'document';
  if (['txt', 'md', 'csv', 'json', 'xml', 'log', 'srt', 'vtt'].includes(ext)) return 'text';
  return 'other';
}

/** The label a card shows for a result. */
export function browseKindLabel(kind: BrowseKind): string {
  switch (kind) {
    case 'image':
      return 'Image';
    case 'audio':
      return 'Audio';
    case 'video':
      return 'Video';
    case 'document':
      return 'Document';
    case 'text':
      return 'Text';
    default:
      return 'File';
  }
}

/**
 * A site crawl: walk a site looking for files whose name matches a term.
 *
 * Distinct from a browse, which reads *one* page. A crawl follows the site's
 * links, so the file someone wants — a datasheet three clicks in — is found
 * without them having to guess which page lists it.
 *
 * A *streaming* page is the harder case and the reason the extractor is asked:
 * its media URL is not among its links at all, so a walk that only reads links
 * reports the channel rather than the video.
 */

/**
 * One rendition a resolved stream offers.
 *
 * A stream is not one file: it is a set of renditions, and which one to take is
 * the viewer's decision — a phone on mobile data does not want the 4K master.
 */
export interface CrawlQuality {
  height: number;
  /** A label to show, e.g. "720p". */
  label: string;
  /** Bytes for this choice, or -1 when the extractor did not say. */
  bytes: number;
  /** True for the largest offered, so the console can say "Best". */
  best: boolean;
}

export interface CrawlHit {
  /** The bytes a download would use. */
  url: string;
  /** The name a save should use. */
  filename: string;
  /** The link text, when the reference was an anchor. Usually more readable
   *  than the filename. */
  title: string | null;
  mime: string;
  /**
   * How well the name matched, 0-100.
   *
   * The crawl returns best-first, so the first result is the likely answer.
   * Shown because a term like "datasheet" legitimately matches a hundred files
   * and the score is what says which one was asked for.
   */
  score: number;
  /** The page this was found on, so a result can be traced back. */
  foundOn: string | null;
  /**
   * True when this page had to be driven through the browser to see it.
   *
   * Worth surfacing: it explains why a crawl took a while, and it is the signal
   * that the site builds its listing in JavaScript.
   */
  viaBrowser: boolean;
  /**
   * True when a media extractor resolved this page, so the hit is the *media*
   * rather than a link the walk found.
   *
   * A streaming page's own links match a search just as well as its video, so
   * this is also what ranks it first.
   */
  resolved: boolean;
  /**
   * The renditions a resolved stream offers, largest first, or empty.
   *
   * Empty for an ordinary file, which has no choice to make.
   */
  qualities: CrawlQuality[];
}

/**
 * What the walk did, whether or not it found anything.
 *
 * Kept separate from the hits so an empty result can be *explained* rather than
 * just reported: "40 pages read, 12 needed the browser, none matched" is a
 * different answer from "that host refused us", and the user's next move
 * depends on which it was.
 */
export interface CrawlReport {
  /** How many pages were read. */
  pagesRead: number;
  /** How many could not be fetched at all. */
  pagesFailed: number;
  /** How many needed the browser to read. */
  pagesRendered: number;
  /** Links on another host that were not followed. */
  skippedOffsite: number;
}

export interface CrawlResult {
  hits: CrawlHit[];
  report: CrawlReport;
  /** The term that was searched for, echoed back. */
  term: string;
  /** The site that was walked. */
  url: string;
}

/**
 * One item of a playlist, channel or search.
 *
 * A *list* — a channel, a playlist, a search — is returned without resolving
 * each item, so an entry carries what the list knew: a title, an id, a page URL,
 * a duration. Resolving it to streams is a second call, made when the viewer
 * chooses one, because resolving a hundred items to show a hundred titles would
 * cost a hundred times as much for nothing.
 */
export interface ExtractEntry {
  id: string | null;
  title: string;
  /** The page URL a second call would resolve. */
  url: string;
  uploader: string | null;
  thumbnail: string | null;
  /** -1 when the list did not say. */
  durationSeconds: number;
  /** True when this entry is itself a playlist rather than a video. */
  isPlaylist: boolean;
}

/** What the extractor found: a single item, or a list of them. */
export interface ExtractResult {
  /** The page resolved, when one was. */
  url: string | null;
  title: string | null;
  /** The site it came from, e.g. "youtube". */
  extractor: string | null;
  durationSeconds: number;
  thumbnail: string | null;
  /** The renditions of a single resolved item, largest first. */
  qualities: CrawlQuality[];
  /** The items of a list. Empty for a single item. */
  entries: ExtractEntry[];
}

/** What to ask the extractor. */
export interface ExtractQuery {
  /** A page to resolve, or a term to search for. One of the two. */
  url?: string;
  search?: string;
  /** How many search results. */
  limit?: number;
  /** List a playlist or channel rather than resolve one item. */
  enumerate?: boolean;
  /** Only items from this date, as YYYYMMDD. */
  dateAfter?: string;
  dateBefore?: string;
  /** Which entries to take, in the extractor's syntax: "1:5", "-20:". */
  playlistItems?: string;
  /** A filter the extractor applies while listing: `duration > 600`. */
  matchFilter?: string;
}

export interface CrawlQuery {
  /** The site to walk. */
  url: string;
  /** The filename to look for. Empty lists every file found. */
  term: string;
  /** How many links deep to follow. 0 reads only the page given. */
  depth: number;
  /** The most pages to read, a stop on a site with a large index. */
  maxPages: number;
  /** Whether a page with no match may be re-read through the browser. */
  useBrowser: boolean;
  /**
   * The most pages to drive a browser for.
   *
   * A browser costs several seconds a page, so the crawl bounds that cost by
   * counting rather than by guessing which page needs one.
   */
  maxBrowserPages: number;
}
