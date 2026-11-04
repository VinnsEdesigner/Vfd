/**
 * A live, driveable page session.
 *
 * This is not a query or a mutation: a session is an *imperative* resource with a
 * lifetime the user controls (open, drive, close), so it is a hook over a
 * `LiveSession` handle rather than React Query. The hook owns the one thing the
 * component cannot: revoking the previous frame's blob URL, which would leak
 * otherwise because a screencast produces a new image many times a second.
 *
 * A live session is expensive — a whole browser held open — so it is opened when
 * the user asks and closed when they leave, never on a mount.
 *
 * REPLACING A SESSION IS SERIALISED
 *
 * Opening a second page while one is live closes the first and waits for its
 * socket to actually close before asking for the next. Connecting immediately
 * races the engine: it releases the browser on its own thread, so a new request
 * that arrives first is refused as busy even though the old session is gone. A
 * fallback timer covers a socket whose close event never arrives.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { useMediaApi } from './provider';
import type { LiveFailure, LiveFrame, LiveMessage, LivePageState } from '../domain/browse';
import type { LiveSession } from '../rest/browse';

export type LiveStatus = 'idle' | 'connecting' | 'open' | 'closed';

/** How long to wait for a replaced session's socket to close before starting the
 *  next one anyway. */
const REPLACE_FALLBACK_MS = 1200;

export interface UseLiveSession {
  status: LiveStatus;
  /** The latest frame. A still (from a screenshot) replaces a stream frame. */
  frame: LiveFrame | null;
  /** The page's own state, as the browser reports it. */
  page: LivePageState | null;
  /** Why the session ended, when it did. Null while it is alive or idle. */
  failure: LiveFailure | null;
  /** A sentence describing the failure or close. */
  reason: string | null;
  /** Open a session on a URL. Replaces any session already open. */
  open: (url: string, width: number, height: number) => void;
  /** Close the session deliberately. */
  close: () => void;
  /** Send a command to the engine. */
  send: (message: unknown) => void;
}

export function useLiveSession(): UseLiveSession {
  const { api } = useMediaApi();
  const [status, setStatus] = useState<LiveStatus>('idle');
  const [frame, setFrame] = useState<LiveFrame | null>(null);
  const [page, setPage] = useState<LivePageState | null>(null);
  const [failure, setFailure] = useState<LiveFailure | null>(null);
  const [reason, setReason] = useState<string | null>(null);

  const sessionRef = useRef<LiveSession | null>(null);
  /** The previous frame's blob URL, so it can be revoked when replaced. */
  const frameUrlRef = useRef<string | null>(null);
  /** An open request waiting for the previous session to close. */
  const pendingRef = useRef<{ url: string; width: number; height: number } | null>(null);
  /** The start function, in a ref so the close handler can reach it without a
   *  cycle between `start` and `open`. */
  const startRef = useRef<((url: string, width: number, height: number) => void) | null>(null);

  /** Swap in a new frame and revoke the old blob. */
  const acceptFrame = useCallback((f: LiveFrame) => {
    if (frameUrlRef.current) URL.revokeObjectURL(frameUrlRef.current);
    frameUrlRef.current = f.url;
    setFrame(f);
  }, []);

  const revokeFrame = useCallback(() => {
    if (frameUrlRef.current) {
      URL.revokeObjectURL(frameUrlRef.current);
      frameUrlRef.current = null;
    }
    setFrame(null);
  }, []);

  const start = useCallback(
    (url: string, width: number, height: number) => {
      setStatus('connecting');
      setPage(null);
      setFailure(null);
      setReason(null);

      const session = api.live.open(url, width, height, {
        onOpen: () => setStatus('open'),
        onFrame: acceptFrame,
        onMessage: (m: LiveMessage) => {
          if (m.kind === 'state') setPage(m.page);
          else if (m.kind === 'closed') {
            setFailure('closed');
            setReason(m.reason);
          } else if (m.kind === 'error') {
            setFailure('failed');
            setReason(m.message);
          }
        },
        onClose: (f: LiveFailure, r: string) => {
          sessionRef.current = null;
          setStatus('closed');
          setFailure(f);
          setReason(r);
          // A replacement that was waiting for this socket to go can start now.
          const pending = pendingRef.current;
          if (pending) {
            pendingRef.current = null;
            startRef.current?.(pending.url, pending.width, pending.height);
          }
        },
      });
      sessionRef.current = session;
      session.connect();
    },
    [api.live, acceptFrame],
  );

  // Keep the ref current so the close handler always calls the latest `start`.
  useEffect(() => {
    startRef.current = start;
  }, [start]);

  const open = useCallback(
    (url: string, width: number, height: number) => {
      const existing = sessionRef.current;
      if (!existing) {
        start(url, width, height);
        return;
      }
      // Replace: remember what to open, drop the old frame, and close. The next
      // session starts when this socket's close arrives, so the engine has
      // released its slot by then.
      pendingRef.current = { url, width, height };
      revokeFrame();
      setStatus('connecting');
      setPage(null);
      setFailure(null);
      setReason(null);
      existing.close();
      window.setTimeout(() => {
        const pending = pendingRef.current;
        if (pending) {
          pendingRef.current = null;
          startRef.current?.(pending.url, pending.width, pending.height);
        }
      }, REPLACE_FALLBACK_MS);
    },
    [revokeFrame, start],
  );

  const close = useCallback(() => {
    pendingRef.current = null;
    sessionRef.current?.close();
    sessionRef.current = null;
    revokeFrame();
    setStatus('closed');
    setFailure('closed');
    setReason('the session was closed');
  }, [revokeFrame]);

  const send = useCallback((message: unknown) => {
    sessionRef.current?.send(message);
  }, []);

  // A session left open when the component goes away would be a browser held for
  // nobody, so it is closed on unmount.
  useEffect(
    () => () => {
      pendingRef.current = null;
      sessionRef.current?.close();
      sessionRef.current = null;
      if (frameUrlRef.current) URL.revokeObjectURL(frameUrlRef.current);
    },
    [],
  );

  return { status, frame, page, failure, reason, open, close, send };
}
