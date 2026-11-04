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

/**
 * Whether this device is driven by touch.
 *
 * Used to tell the engine to emulate a touch device, so the page lays itself out
 * the way it would in any other browser on this device and a forwarded finger
 * behaves as one. A coarse pointer is the reliable signal: a phone and a tablet
 * report it, a desktop with a mouse does not.
 */
export function isTouchDevice(): boolean {
  if (typeof window === 'undefined' || !window.matchMedia) return false;
  return window.matchMedia('(pointer: coarse)').matches || 'ontouchstart' in window;
}

/**
 * The viewport to give the engine on a touch device.
 *
 * Deliberately *fixed*, not the stage's size. On a phone the stage's height
 * changes constantly — the URL bar hides as you scroll, the on-screen keyboard
 * opens and closes — and re-fitting the browser on every change makes the page
 * reflow under your finger, which is the awkward behaviour this replaces. A
 * fixed phone-shaped viewport is what every other mobile browser gives a page.
 *
 * The device's own CSS viewport is used when it is available and sane, so the
 * page sees the same size it would in the phone's browser; a floor and ceiling
 * keep a stray value (a desktop-class width reported as coarse, a 0 during
 * startup) from producing a silly layout.
 */
export function mobileViewport(): { width: number; height: number } {
  const fallback = { width: 390, height: 844 };
  if (typeof window === 'undefined') return fallback;
  const w = Math.round(window.innerWidth);
  const h = Math.round(window.innerHeight);
  if (!Number.isFinite(w) || !Number.isFinite(h) || w < 240 || h < 320) return fallback;
  return { width: Math.min(w, 768), height: Math.min(h, 1024) };
}

export interface UseLiveSession {
  status: LiveStatus;
  /** The latest frame. A still (from a screenshot) replaces a stream frame. */
  frame: LiveFrame | null;
  /** The page's own state, as the browser reports it. */
  page: LivePageState | null;
  /**
   * The size the engine is drawing at, once a session is open.
   *
   * The view locks its stage to this so the frame is shown at its natural shape
   * rather than being stretched or letterboxed into whatever space is available —
   * which is what makes the coordinates a tap maps to correct.
   */
  viewport: { width: number; height: number } | null;
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
  const [viewport, setViewport] = useState<{ width: number; height: number } | null>(null);
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

      /*
       * On a touch device the viewport is fixed, not the caller's size.
       *
       * The caller passes the stage's size, which on a phone changes as the URL
       * bar and the on-screen keyboard come and go. Giving the browser that size
       * would make the page reflow under the user's finger; a fixed phone-shaped
       * viewport is what every other mobile browser does.
       */
      const touch = isTouchDevice();
      const vp = touch ? mobileViewport() : { width, height };
      setViewport(vp);

      const session = api.live.open(url, vp.width, vp.height, {
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
      }, isTouchDevice());
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

  return { status, viewport, frame, page, failure, reason, open, close, send };
}
