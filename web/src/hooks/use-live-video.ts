/**
 * A live video session, as a hook.
 *
 * Imperative, like the JPEG live hook: a session is a resource with a lifetime
 * the user controls, not a query. The hook owns the things a component cannot:
 * closing the session on unmount, so a browser is not held open for nobody, and
 * keeping the session in a ref so a re-render does not open a second one.
 *
 * The <video> element is owned by the view and handed to `open`, because the
 * element has to be in the DOM before media can be attached to it.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { useMediaApi } from './provider';
import type { LiveVideoState, LiveVideoViewport } from '../domain/rtc';
import type { LiveVideoSession } from '../rest/rtc-session';

export type LiveVideoStatus = 'idle' | 'connecting' | 'live' | 'ended';

export interface UseLiveVideo {
  status: LiveVideoStatus;
  /** The connection's own state, for a detail line. */
  connection: LiveVideoState;
  /** The viewport Vyzor is capturing, once known. */
  viewport: LiveVideoViewport | null;
  /** Why the session ended, when it did. */
  reason: string | null;
  /** A signalling or transport error worth showing. */
  error: string | null;
  /** Open a session on a URL, attaching media to `video`. Replaces any session
   *  already open. */
  open: (url: string, video: HTMLVideoElement) => void;
  /** Close deliberately. */
  close: () => void;
  /** Send an input command. A no-op when nothing is open. */
  send: (message: unknown) => void;
}

export function useLiveVideo(): UseLiveVideo {
  const { api } = useMediaApi();
  const [status, setStatus] = useState<LiveVideoStatus>('idle');
  const [connection, setConnection] = useState<LiveVideoState>('new');
  const [viewport, setViewport] = useState<LiveVideoViewport | null>(null);
  const [reason, setReason] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const sessionRef = useRef<LiveVideoSession | null>(null);

  const open = useCallback(
    (url: string, video: HTMLVideoElement) => {
      // Replace: Vyzor's gate admits one session, so the old one has to go
      // before the new one is asked for.
      sessionRef.current?.close();
      sessionRef.current = null;

      setStatus('connecting');
      setConnection('new');
      setViewport(null);
      setReason(null);
      setError(null);

      const session = api.rtc.open(url, video, {
        onOffer: (vp) => setViewport(vp),
        onState: (state, connected) => {
          setConnection(state);
          if (connected) setStatus('live');
        },
        onLive: () => setStatus('live'),
        onError: (message) => setError(message),
        onEnded: (why) => {
          sessionRef.current = null;
          setStatus('ended');
          setReason(why);
        },
      });
      sessionRef.current = session;
      void session.connect(url);
    },
    [api.rtc],
  );

  const close = useCallback(() => {
    sessionRef.current?.close();
    sessionRef.current = null;
    setStatus('ended');
    setConnection('closed');
    setReason('the session was closed');
  }, []);

  const send = useCallback((message: unknown) => {
    sessionRef.current?.send(message);
  }, []);

  // A session left open when the component goes away would be a browser held for
  // nobody, so it is closed on unmount.
  useEffect(
    () => () => {
      sessionRef.current?.close();
      sessionRef.current = null;
    },
    [],
  );

  return { status, connection, viewport, reason, error, open, close, send };
}
