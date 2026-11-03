/**
 * Sharing via the Web Share API.
 *
 * Three tiers, best first, because support varies by browser:
 *   1. share the actual files (navigator.share({ files })) — the bytes go to the
 *      target app, so "share with Photos" really sends the photo;
 *   2. share a URL to the content — the fallback when files are unsupported;
 *   3. copy to the clipboard — the last resort, and it tells the user it did.
 *
 * `navigator.share` can only be called from a user gesture, so this must stay
 * synchronous up to that call. Fetching the blobs first would break that on
 * some browsers, so the file tier is attempted only when `canShare` already
 * reports support and the fetch is awaited immediately before the call.
 */

export interface ShareTarget {
  id: string;
  name: string;
  /** Absolute URL of the bytes. */
  url: string;
  mime: string;
}

export type ShareMethod = 'files' | 'url' | 'clipboard' | 'cancelled' | 'download';

export interface ShareOutcome {
  method: ShareMethod;
  count: number;
}

/** A MIME type the target app can act on. Falls back to octet-stream. */
const MIME_BY_EXT: Record<string, string> = {
  mp4: 'video/mp4',
  m4v: 'video/mp4',
  webm: 'video/webm',
  mkv: 'video/x-matroska',
  avi: 'video/x-msvideo',
  mov: 'video/quicktime',
  ts: 'video/mp2t',
  ogv: 'video/ogg',
  jpg: 'image/jpeg',
  jpeg: 'image/jpeg',
  png: 'image/png',
  gif: 'image/gif',
  webp: 'image/webp',
  bmp: 'image/bmp',
  tiff: 'image/tiff',
  avif: 'image/avif',
  heic: 'image/heic',
  mp3: 'audio/mpeg',
  m4a: 'audio/mp4',
  aac: 'audio/aac',
  flac: 'audio/flac',
  wav: 'audio/wav',
  ogg: 'audio/ogg',
};

export function mimeForName(name: string): string {
  const dot = name.lastIndexOf('.');
  const ext = dot > 0 ? name.slice(dot + 1).toLowerCase() : '';
  return MIME_BY_EXT[ext] ?? 'application/octet-stream';
}

/** The browser's share sheet, if it has one at all. */
function shareApi(): ((data: ShareData) => Promise<void>) | null {
  if (typeof navigator === 'undefined') return null;
  const nav = navigator as Navigator & { share?: (data: ShareData) => Promise<void> };
  return typeof nav.share === 'function' ? nav.share.bind(nav) : null;
}

function canShareFiles(files: File[]): boolean {
  if (typeof navigator === 'undefined') return false;
  const nav = navigator as Navigator & { canShare?: (data: ShareData) => boolean };
  if (typeof nav.canShare !== 'function') return false;
  try {
    return nav.canShare({ files });
  } catch {
    return false;
  }
}

export function isShareSupported(): boolean {
  return shareApi() !== null;
}

function isAbort(e: unknown): boolean {
  return e instanceof DOMException && e.name === 'AbortError';
}

export interface ShareOptions {
  /** Injectable for tests; defaults to the platform fetch. */
  fetchImpl?: typeof fetch;
}

/**
 * Share one or more items. Throws only for a real failure; a user cancelling the
 * share sheet resolves with `{ method: 'cancelled' }`, because that is not an
 * error to report.
 */
export async function shareItems(
  targets: readonly ShareTarget[],
  options: ShareOptions = {},
): Promise<ShareOutcome> {
  if (targets.length === 0) throw new Error('nothing to share');
  const share = shareApi();
  const first = targets[0]!;

  if (share && targets.length === 1) {
    const fetcher = options.fetchImpl ?? globalThis.fetch;
    // Only pay for the download when the platform will actually take a file.
    const probe = new File([new Blob([])], first.name, { type: first.mime });
    if (canShareFiles([probe])) {
      try {
        const res = await fetcher(first.url);
        if (res.ok) {
          const blob = await res.blob();
          const file = new File([blob], first.name, { type: first.mime || blob.type });
          if (canShareFiles([file])) {
            await share({ files: [file], title: first.name });
            return { method: 'files', count: 1 };
          }
        }
      } catch (e) {
        if (isAbort(e)) return { method: 'cancelled', count: 0 };
        // Fall through to the URL tier rather than failing the whole share.
      }
    }

    try {
      await share({ title: first.name, url: first.url });
      return { method: 'url', count: 1 };
    } catch (e) {
      if (isAbort(e)) return { method: 'cancelled', count: 0 };
      throw e;
    }
  }

  if (share && targets.length > 1) {
    try {
      await share({
        title: `${targets.length} items`,
        text: targets.map((t) => t.name).join('\n'),
      });
      return { method: 'url', count: targets.length };
    } catch (e) {
      if (isAbort(e)) return { method: 'cancelled', count: 0 };
      // Fall through to the clipboard.
    }
  }

  // Last resort: the links are still useful pasted anywhere.
  if (typeof navigator !== 'undefined' && navigator.clipboard) {
    await navigator.clipboard.writeText(targets.map((t) => t.url).join('\n'));
    return { method: 'clipboard', count: targets.length };
  }

  throw new Error('sharing is not available in this browser');
}

/* ------------------------------------------------------------------ */
/* Opening in another app                                             */
/* ------------------------------------------------------------------ */

export interface OpenWithResult {
  /**
   * How the handoff happened:
   *   files    the bytes went to the chosen app (the real "open with" sheet);
   *   url      the platform was given the URL to route;
   *   download the browser took the file instead — no app chooser exists here.
   */
  method: 'files' | 'url' | 'download';
  /** The message the caller can put in front of the user. */
  message: string;
}

/**
 * Hand a file to another app on the device.
 *
 * There is no "open with" API on the web, so this is the share sheet with an
 * explicit intent: sharing the *bytes* is what makes Android offer its app
 * chooser (VLC, Photos, a messaging app…). When the platform cannot take files,
 * the file is downloaded instead — which is the same gesture the OS "Open with"
 * would end in — rather than the button pretending to have launched something.
 */
export async function openWithItem(
  target: ShareTarget,
  options: ShareOptions = {},
): Promise<OpenWithResult> {
  const share = shareApi();
  const fetcher = options.fetchImpl ?? globalThis.fetch;

  if (share) {
    // The file tier is what produces an app chooser on Android.
    const probe = new File([new Blob([])], target.name, { type: target.mime });
    if (canShareFiles([probe])) {
      try {
        const res = await fetcher(target.url);
        if (res.ok) {
          const blob = await res.blob();
          const file = new File([blob], target.name, { type: target.mime || blob.type });
          if (canShareFiles([file])) {
            await share({ files: [file], title: target.name });
            return { method: 'files', message: `Sent ${target.name} to the app you chose.` };
          }
        }
      } catch (e) {
        if (isAbort(e)) return { method: 'files', message: '' };
        // Fall through: try the URL tier, then the download.
      }
    }
    try {
      await share({ title: target.name, url: target.url });
      return { method: 'url', message: `Shared ${target.name} as a link.` };
    } catch (e) {
      if (isAbort(e)) return { method: 'files', message: '' };
    }
  }

  triggerDownload(target.url, target.name);
  return {
    method: 'download',
    message: 'This browser has no app picker, so the file was downloaded instead.',
  };
}

/** Save a URL under a chosen filename, without leaving the page. */
export function triggerDownload(url: string, filename: string): void {
  if (typeof document === 'undefined') return;
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.rel = 'noopener';
  document.body.appendChild(a);
  a.click();
  a.remove();
}
