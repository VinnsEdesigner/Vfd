/**
 * InteractiveView — a page you can actually use.
 *
 * This replaces a picture of a page with the page itself: the engine keeps a
 * browser alive, streams what it draws, and forwards the input this view
 * captures. A search box can be typed in, a link followed, a page scrolled —
 * none of which a screenshot can do.
 *
 * WHAT THIS VIEW OWNS
 *
 *   the canvas        an <img> showing the latest frame
 *   the input bridge  DOM events, mapped to the engine's commands
 *   the sizing        keeping the browser's viewport in step with the stage
 *
 * WHAT IT DOES NOT OWN
 *
 *   the socket        that is the `useLiveSession` hook, passed in as `live`
 *   the engine        the engine decides whether a page may be driven at all
 *
 * A page driven here runs with the engine's browser, unsandboxed (an
 * unprivileged container cannot use Chromium's namespace sandbox). That is why
 * the engine only opens pages the operator would be willing to visit, and why
 * this view says plainly that the page is live.
 */

import { useCallback, useEffect, useRef } from 'react';
import { Button } from '../primitives/Button';
import { Spinner } from '../primitives/Spinner';
import { Note } from '../primitives/Note';
import { EmptyState } from '../primitives/EmptyState';
import { IconRefresh, IconExternal, IconX, IconImage } from '../primitives/Icons';
import type { LiveFailure, LiveFrame, LivePageState } from '../../hooks';
import {
  keyMessage,
  modifiersFromEvent,
  mouseMessage,
  resizeMessage,
  textMessage,
  toFrameCoords,
  wheelMessage,
  type LiveKeyMessage,
} from './live-input';

export type LiveStatus = 'idle' | 'connecting' | 'open' | 'closed';

export interface LiveViewProps {
  status: LiveStatus;
  frame: LiveFrame | null;
  page: LivePageState | null;
  failure: LiveFailure | null;
  reason: string | null;
  onOpen: (url: string, width: number, height: number) => void;
  onClose: () => void;
  send: (message: unknown) => void;
}

export interface InteractiveViewProps {
  /** The address being driven, as typed. */
  url: string;
  live: LiveViewProps;
}

/** The label for a failure, in the words a person would use. */
function failureLabel(f: LiveFailure): string {
  switch (f) {
    case 'disabled':
      return 'Live pages are turned off on this engine';
    case 'unavailable':
      return 'This engine has no browser installed, so it cannot drive a page';
    case 'busy':
      return 'The engine is already driving another page — try again in a moment';
    case 'refused':
      return 'That address cannot be driven — only public http(s) pages can';
    case 'closed':
      return 'The session ended';
    default:
      return 'That page could not be opened';
  }
}

function failureDetail(f: LiveFailure): string {
  switch (f) {
    case 'disabled':
      return 'The engine has live sessions turned off. It is started with --interactive on a host that has a browser.';
    case 'unavailable':
      return 'Driving a page needs a browser on the engine host. The engine reports this in its capabilities, so this control is only offered where it works.';
    case 'busy':
      return 'Only a few pages can be driven at once, because each one holds a whole browser open. Nothing is queued — close the other session or ask again shortly.';
    case 'refused':
      return 'Only public http(s) pages are driven. A file:// path would read the engine’s disk, and loopback or link-local addresses are refused.';
    case 'closed':
      return 'The engine closed the session — a page that sat idle is closed rather than left running, and a page that crashed is reported here.';
    default:
      return 'The engine’s browser could not open that page.';
  }
}

/** The button name CDP expects, from the DOM's numeric button. */
function buttonName(button: number): 'left' | 'middle' | 'right' | 'none' {
  if (button === 0) return 'left';
  if (button === 1) return 'middle';
  if (button === 2) return 'right';
  return 'none';
}

export function InteractiveView({ url, live }: InteractiveViewProps) {
  const { status, frame, page, failure, reason, onOpen, onClose, send } = live;
  const stageRef = useRef<HTMLDivElement | null>(null);
  const imgRef = useRef<HTMLImageElement | null>(null);
  /** The last viewport we told the engine about, so we do not repeat ourselves. */
  const sentSize = useRef<{ w: number; h: number }>({ w: 0, h: 0 });
  const isLive = status === 'open' || status === 'connecting';

  /**
   * Tell the engine the size the page should be drawn at, so its frames match
   * the stage rather than being scaled and blurry. Debounced through a frame, and
   * skipped when the size has not really changed, because a resize observer
   * fires for sub-pixel jitter.
   */
  const pushSize = useCallback(
    (width: number, height: number) => {
      const w = Math.round(width);
      const h = Math.round(height);
      if (w < 64 || h < 64) return;
      if (w === sentSize.current.w && h === sentSize.current.h) return;
      sentSize.current = { w, h };
      send(resizeMessage(w, h));
    },
    [send],
  );

  // Keep the browser's viewport in step with the stage.
  useEffect(() => {
    const el = stageRef.current;
    if (!el || !isLive) return;
    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) {
        pushSize(entry.contentRect.width, entry.contentRect.height);
      }
    });
    observer.observe(el);
    // An initial size, in case the observer's first callback is delayed.
    pushSize(el.clientWidth, el.clientHeight);
    return () => observer.disconnect();
  }, [isLive, pushSize]);

  /**
   * A wheel must be forwarded and its default stopped, or the page scrolls the
   * console instead of the driven page. React's `onWheel` is passive, so this is
   * a native listener with `passive: false`.
   */
  useEffect(() => {
    const el = stageRef.current;
    if (!el || !isLive) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const img = imgRef.current;
      if (!img) return;
      const { x, y } = toFrameCoords(
        e.clientX,
        e.clientY,
        img.getBoundingClientRect(),
        frame?.width ?? 0,
        frame?.height ?? 0,
      );
      send(wheelMessage(x, y, e.deltaX, e.deltaY, modifiersFromEvent(e)));
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, [isLive, frame?.width, frame?.height, send]);

  const frameCoords = useCallback(
    (clientX: number, clientY: number) => {
      const img = imgRef.current;
      if (!img) return { x: 0, y: 0 };
      return toFrameCoords(
        clientX,
        clientY,
        img.getBoundingClientRect(),
        frame?.width ?? 0,
        frame?.height ?? 0,
      );
    },
    [frame?.width, frame?.height],
  );

  const onMouseDown = useCallback(
    (e: React.MouseEvent) => {
      if (!isLive) return;
      const { x, y } = frameCoords(e.clientX, e.clientY);
      send(mouseMessage('mousePressed', x, y, buttonName(e.button), 1, e.buttons, modifiersFromEvent(e)));
      // The stage needs focus for keys to reach the page.
      stageRef.current?.focus();
    },
    [isLive, frameCoords, send],
  );

  const onMouseUp = useCallback(
    (e: React.MouseEvent) => {
      if (!isLive) return;
      const { x, y } = frameCoords(e.clientX, e.clientY);
      send(mouseMessage('mouseReleased', x, y, buttonName(e.button), 1, e.buttons, modifiersFromEvent(e)));
    },
    [isLive, frameCoords, send],
  );

  const onMouseMove = useCallback(
    (e: React.MouseEvent) => {
      if (!isLive || e.buttons === 0) return; // only forward a drag, not idle motion
      const { x, y } = frameCoords(e.clientX, e.clientY);
      send(mouseMessage('mouseMoved', x, y, buttonName(e.button), 0, e.buttons, modifiersFromEvent(e)));
    },
    [isLive, frameCoords, send],
  );

  const onKeyDown = useCallback(
    (e: React.KeyboardEvent) => {
      if (!isLive) return;
      // Escape leaves the stage, so the console stays usable by keyboard.
      if (e.key === 'Escape') {
        stageRef.current?.blur();
        return;
      }
      // Let the console keep its own reload, so the user is never trapped.
      if (e.key === 'F5' || (e.ctrlKey && e.key === 'r')) return;
      e.preventDefault();
      const event: LiveKeyMessage['event'] = 'keyDown';
      send(
        keyMessage(event, {
          key: e.key,
          code: e.code,
          keyCode: e.keyCode,
          altKey: e.altKey,
          ctrlKey: e.ctrlKey,
          metaKey: e.metaKey,
          shiftKey: e.shiftKey,
        }),
      );
    },
    [isLive, send],
  );

  const onKeyUp = useCallback(
    (e: React.KeyboardEvent) => {
      if (!isLive) return;
      send(
        keyMessage('keyUp', {
          key: e.key,
          code: e.code,
          keyCode: e.keyCode,
          altKey: e.altKey,
          ctrlKey: e.ctrlKey,
          metaKey: e.metaKey,
          shiftKey: e.shiftKey,
        }),
      );
    },
    [isLive, send],
  );

  /** Committed text (paste), which is not a key sequence. */
  const onPaste = useCallback(
    (e: React.ClipboardEvent) => {
      if (!isLive) return;
      const text = e.clipboardData.getData('text');
      if (text) {
        e.preventDefault();
        send(textMessage(text));
      }
    },
    [isLive, send],
  );

  const shownFrame = frame;
  const liveUrl = page?.url || url;

  return (
    <div className="live">
      <div className="live__bar">
        <span className="live__url" title={liveUrl}>
          {liveUrl}
        </span>
        {page?.loading && <span className="live__loading">Loading…</span>}
        <div className="live__actions">
          <Button size="sm" onClick={() => send({ type: 'reload' })} disabled={!isLive}>
            <IconRefresh /> Reload
          </Button>
          <Button
            size="sm"
            onClick={() => send({ type: 'screenshot' })}
            disabled={!isLive}
            title="Ask the engine for a full-resolution still"
          >
            <IconImage /> Still
          </Button>
          <Button size="sm" onClick={onClose} disabled={!isLive}>
            <IconX /> Stop
          </Button>
          <a className="btn btn--sm" href={liveUrl} target="_blank" rel="noreferrer">
            <IconExternal /> New tab
          </a>
        </div>
      </div>

      {status === 'idle' && (
        <EmptyState
          title="Open this page live"
          description="The engine keeps a browser on this page so you can use it — type in it, follow a link, scroll. Press Open to start; nothing runs until you do."
        />
      )}

      {status === 'connecting' && (
        <div className="live__waiting">
          <Spinner size="lg" />
          <p>The engine is opening this page in a browser…</p>
        </div>
      )}

      {status === 'closed' && failure && (
        <>
          <EmptyState title={failureLabel(failure)} description={failureDetail(failure)} />
          <div className="live__retry">
            <Button variant="primary" onClick={() => onOpen(url, 1024, 768)}>
              Open again
            </Button>
          </div>
        </>
      )}

      {isLive && (
        <>
          {/* A page driven by the engine's browser runs unsandboxed, so say so
              rather than let it look like an ordinary frame. */}
          <Note className="live__note">
            This page is being driven by the engine’s own browser. It is not sandboxed — only open
            pages you would be willing to visit.
          </Note>
          <div
            className="live__stage"
            ref={stageRef}
            tabIndex={0}
            role="application"
            aria-label={`Live page ${liveUrl}`}
            onMouseDown={onMouseDown}
            onMouseUp={onMouseUp}
            onMouseMove={onMouseMove}
            onKeyDown={onKeyDown}
            onKeyUp={onKeyUp}
            onPaste={onPaste}
          >
            {shownFrame ? (
              <img
                ref={imgRef}
                className="live__frame"
                src={shownFrame.url}
                alt={`The engine’s view of ${liveUrl}`}
                draggable={false}
              />
            ) : (
              <div className="live__blank">
                <Spinner />
                <span>Waiting for the first frame…</span>
              </div>
            )}
          </div>
          <p className="live__hint">
            Click to focus, then type. Escape releases the keyboard. Scrolling is forwarded to the
            page.
          </p>
        </>
      )}

      {reason && status === 'closed' && <Note className="note note--warn">{reason}</Note>}
    </div>
  );
}
