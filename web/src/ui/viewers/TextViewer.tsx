/**
 * TextViewer — a plain-text file, fetched and shown.
 *
 * Fetched rather than framed: a .txt has no renderer, and an iframe would either
 * download it or show nothing. The response is read as text and rendered into a
 * <pre>, with a size ceiling so a 50 MB log does not lock the tab.
 *
 * The fetch is bounded and failure is reported: a blank pane that looks like an
 * empty file is worse than a message saying it could not be read.
 */

import { useEffect, useState } from 'react';
import { Note } from '../primitives/Note';
import { Spinner } from '../primitives/Spinner';

export interface TextViewerProps {
  src: string;
  title: string;
}

/** Beyond this we show the start and say so, rather than freezing the tab. */
const MAX_BYTES = 512 * 1024;

export function TextViewer({ src, title }: TextViewerProps) {
  const [state, setState] = useState<
    | { status: 'loading' }
    | { status: 'ready'; text: string; truncated: boolean }
    | { status: 'failed'; reason: string }
  >({ status: 'loading' });

  useEffect(() => {
    let cancelled = false;
    setState({ status: 'loading' });
    void (async () => {
      try {
        const res = await fetch(src);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const buf = await res.arrayBuffer();
        const truncated = buf.byteLength > MAX_BYTES;
        const slice = truncated ? buf.slice(0, MAX_BYTES) : buf;
        if (cancelled) return;
        setState({ status: 'ready', text: new TextDecoder().decode(slice), truncated });
      } catch (e) {
        if (cancelled) return;
        setState({
          status: 'failed',
          reason: e instanceof Error ? e.message : 'the file could not be read',
        });
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [src]);

  if (state.status === 'loading') {
    return (
      <div className="textviewer textviewer--centre">
        <Spinner size="lg" />
      </div>
    );
  }

  if (state.status === 'failed') {
    return (
      <div className="textviewer">
        <Note className="note note--warn">Could not read {title}: {state.reason}</Note>
      </div>
    );
  }

  return (
    <div className="textviewer">
      {state.truncated && (
        <Note className="note">
          Showing the first {Math.round(MAX_BYTES / 1024)} KB of {title}.
        </Note>
      )}
      <pre className="textviewer__body">{state.text}</pre>
    </div>
  );
}
