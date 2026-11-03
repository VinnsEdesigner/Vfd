/**
 * What an in-app search can find.
 *
 * These are *candidates* — files that live somewhere else on the internet and can
 * be fetched. They are not library items: nothing here is on this device or in
 * the engine until the user asks for it. Keeping the two apart is why the browse
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
