/**
 * useLogs — the server's recent log lines, polled incrementally.
 *
 * The server logs to stderr, which the console cannot see, so without this a
 * user watching a crawl or a download fail is blind to the reason. The server
 * keeps a bounded in-memory ring and serves it with a monotonic `seq`; this hook
 * asks only for what is newer than the highest seq it already holds, so polling
 * is cheap and no line is shown twice.
 *
 * The accumulation is client-side rather than the server re-sending everything:
 * the ring is bounded, so re-fetching the whole thing each poll would be wasted
 * work, and a line that scrolled out of the ring should stay visible until the
 * user clears it, not vanish on the next poll.
 */

import { useEffect, useRef, useState } from 'react';
import { useMediaApi } from './provider';
import type { LogLine } from '../domain/media';

/* Re-exported so the ui layer can name it without importing domain directly,
 * which the layer boundaries forbid. */
export type { LogLine };

/** How often to ask for new lines. Fast enough to feel live, cheap enough to
 *  leave running: the response is empty when nothing has happened. */
const POLL_MS = 3000;

/** The most lines to keep in the view, so a long session cannot grow the tab
 *  without bound. Matches the server's own ring size. */
const MAX_LINES = 500;

export interface LogsView {
  lines: readonly LogLine[];
  /** Whether the server is capturing lines at all. */
  enabled: boolean;
  /** Whether the first fetch has completed. */
  loaded: boolean;
  /** Why polling failed, when it did — a console that cannot reach Vyzor. */
  error: string | null;
  /** Forget every line held here. The server's ring is untouched. */
  clear: () => void;
}

export function useLogs(active: boolean): LogsView {
  const { api } = useMediaApi();
  const [lines, setLines] = useState<readonly LogLine[]>([]);
  const [enabled, setEnabled] = useState(true);
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState<string | null>(null);
  /* The highest seq seen, kept in a ref so the poll loop reads the latest
   * without the effect re-subscribing on every line. */
  const sinceRef = useRef(0);

  useEffect(() => {
    /* Only poll while the view is on screen. A background tab that kept asking
     * would be a request every few seconds for lines nobody is reading. */
    if (!active) return;
    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;

    const poll = async () => {
      try {
        const snap = await api.system.logs(sinceRef.current);
        if (cancelled) return;
        if (snap.lines.length) {
          sinceRef.current = snap.latest;
          setLines((prev) => {
            const next = [...prev, ...snap.lines];
            return next.length > MAX_LINES ? next.slice(next.length - MAX_LINES) : next;
          });
        } else {
          /* No new lines, but the ring may have advanced (a line that scrolled
           * out between polls). Take the server's latest so the next ask is not
           * from a stale point. */
          if (snap.latest > sinceRef.current) sinceRef.current = snap.latest;
        }
        setEnabled(snap.enabled);
        setError(null);
      } catch (e) {
        if (!cancelled) setError(e instanceof Error ? e.message : 'could not reach Vyzor');
      } finally {
        if (!cancelled) {
          setLoaded(true);
          timer = setTimeout(poll, POLL_MS);
        }
      }
    };

    poll();
    return () => {
      cancelled = true;
      if (timer) clearTimeout(timer);
    };
  }, [active, api]);

  const clear = () => {
    /* Keep `since` so the next poll does not re-deliver everything. Clearing is
     * "I have read these", not "start over". */
    setLines([]);
  };

  return { lines, enabled, loaded, error, clear };
}
