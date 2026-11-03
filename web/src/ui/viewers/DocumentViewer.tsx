/**
 * DocumentViewer — a PDF (or similar) shown in the page.
 *
 * A PDF is rendered by the browser's own viewer, which needs an <iframe> or
 * <object>. The catch: the response must permit framing. Wikimedia's upload host
 * sends `frame-ancestors 'none'` on PDFs, so framing it is blocked — and the
 * failure is invisible, because a refused frame simply renders blank.
 *
 * So this does not assume. It frames the document and watches for the load; if the
 * frame never reports loaded within a short window, it says so and offers the
 * honest alternative (open in a new tab, which always works because it is not a
 * frame).
 */

import { useEffect, useState } from 'react';
import { Note } from '../primitives/Note';

export interface DocumentViewerProps {
  src: string;
  title: string;
  /** Absolute URL for the "open in a new tab" escape hatch. */
  openUrl: string;
}

export function DocumentViewer({ src, title, openUrl }: DocumentViewerProps) {
  const [state, setState] = useState<'loading' | 'shown' | 'blocked'>('loading');

  useEffect(() => {
    setState('loading');
    // A framed document that is refused by the server never fires `load`, so a
    // timeout is the only signal available. Short enough not to feel stuck.
    const timer = window.setTimeout(() => setState((s) => (s === 'loading' ? 'blocked' : s)), 3500);
    return () => window.clearTimeout(timer);
  }, [src]);

  return (
    <div className="docviewer">
      {state === 'blocked' && (
        <Note className="note note--warn">
          This document cannot be shown inside the app — its host does not allow framing.
          Open it in a new tab to read it.
        </Note>
      )}
      <object
        className="docviewer__frame"
        data={src}
        type="application/pdf"
        aria-label={title}
        onLoad={() => setState('shown')}
      >
        {/* Rendered when the browser has no PDF support at all. */}
        <div className="docviewer__fallback">
          <p>This browser cannot display the document inline.</p>
          <a className="btn btn--primary" href={openUrl} target="_blank" rel="noopener noreferrer">
            Open in a new tab
          </a>
        </div>
      </object>
    </div>
  );
}
