/**
 * Which media kinds the browser can actually decode, and the Content-Type the
 * Vyzor will serve for each. Kept in one place so the viewers, the fallback
 * and any future capability check cannot disagree.
 */

export type ViewerKind = 'image' | 'video' | 'audio' | 'document' | 'text' | 'unsupported';

/** Video the browser plays natively. MKV/AVI/FLV/MPEG-TS will not decode. */
const NATIVE_VIDEO = new Set(['mp4', 'webm', 'ogg']);
/** Images the browser decodes. HEIC and TIFF generally will not. */
const NATIVE_IMAGE = new Set(['jpeg', 'png', 'gif', 'webp', 'bmp', 'avif']);
/** Audio the browser plays. */
const NATIVE_AUDIO = new Set(['mp3', 'wav', 'aac', 'ogg-audio', 'flac']);
/** Documents the browser can render itself (PDF) or that have a reader. */
const DOCUMENT_KINDS = new Set(['pdf', 'djvu']);
/** Plain text the app renders into a <pre>. */
const TEXT_KINDS = new Set(['txt', 'text', 'markdown', 'csv', 'json', 'xml', 'srt', 'vtt', 'log']);

export interface ViewerCapability {
  kind: ViewerKind;
  /** False when the container is right but the browser cannot decode it. */
  playable: boolean;
  /** Why it cannot be played, for the fallback message. */
  reason?: string;
}

/**
 * Decide how to present a file. `kindLabel` is the human label from the view
 * model (e.g. "MKV"), normalised to lower case here.
 */
export function viewerFor(file: {
  isVideo: boolean;
  isImage: boolean;
  isAudio: boolean;
  kindLabel: string;
}): ViewerCapability {
  const kind = file.kindLabel.toLowerCase();

  if (file.isImage) {
    return NATIVE_IMAGE.has(kind)
      ? { kind: 'image', playable: true }
      : {
          kind: 'image',
          playable: false,
          reason: `${file.kindLabel} is not decodable in a browser.`,
        };
  }

  if (file.isAudio) {
    return NATIVE_AUDIO.has(kind)
      ? { kind: 'audio', playable: true }
      : {
          kind: 'audio',
          playable: false,
          reason: `${file.kindLabel} is not playable in a browser.`,
        };
  }

  if (file.isVideo) {
    return NATIVE_VIDEO.has(kind)
      ? { kind: 'video', playable: true }
      : {
          kind: 'video',
          playable: false,
          reason: `${file.kindLabel} is not natively playable — transcode to HLS for an in-app preview.`,
        };
  }

  // Documents and text are not "played"; they are rendered. A PDF goes to the
  // browser's own viewer (which may refuse framing — the viewer says so), and
  // plain text is fetched and shown.
  if (DOCUMENT_KINDS.has(kind)) {
    return { kind: 'document', playable: true };
  }
  if (TEXT_KINDS.has(kind)) {
    return { kind: 'text', playable: true };
  }

  return { kind: 'unsupported', playable: false, reason: 'No viewer for this file type.' };
}

/** Formats a duration in seconds as h:mm:ss / m:ss. */
export function formatClock(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return '--:--';
  const total = Math.floor(seconds);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return h > 0
    ? `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
    : `${m}:${String(s).padStart(2, '0')}`;
}
