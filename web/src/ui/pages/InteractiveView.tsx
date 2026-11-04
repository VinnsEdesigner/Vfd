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
  touchMessage,
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
      /*
       * On a touch device the browser also synthesises mouse events from a tap,
       * after the touch handlers have already forwarded the tap and its click. Let
       * those through and the page receives every press twice — a double click,
       * which selects a word instead of placing a caret.
       */
      if (suppressMouseAfterTouch.current) return;
      const { x, y } = frameCoords(e.clientX, e.clientY);
      send(mouseMessage('mousePressed', x, y, buttonName(e.button), 1, e.buttons, modifiersFromEvent(e)));
      // The stage needs focus for keys to reach the page.
      stageRef.current?.focus();
    },
    [isLive, frameCoords, send],
  );

  const onMouseUp = useCallback(
    (e: React.MouseEvent) => {
      if (!isLive || suppressMouseAfterTouch.current) return;
      const { x, y } = frameCoords(e.clientX, e.clientY);
      send(mouseMessage('mouseReleased', x, y, buttonName(e.button), 1, e.buttons, modifiersFromEvent(e)));
    },
    [isLive, frameCoords, send],
  );

  const onMouseMove = useCallback(
    (e: React.MouseEvent) => {
      if (!isLive || suppressMouseAfterTouch.current || e.buttons === 0) return; // only a drag
      const { x, y } = frameCoords(e.clientX, e.clientY);
      send(mouseMessage('mouseMoved', x, y, buttonName(e.button), 0, e.buttons, modifiersFromEvent(e)));
    },
    [isLive, frameCoords, send],
  );

  /*
   * Touch, for a touch device.
   *
   * Mouse events are not enough on a phone or tablet: a finger produces pointer
   * events, and the stage sets `touch-action: none` so the browser does not
   * scroll it. Without forwarding the touches, the stage is inert — no scroll,
   * and no tap to focus a field, so the on-screen keyboard never appears.
   *
   * The mapping is deliberately simple: one or two fingers, tracked by id, sent
   * as the engine's flat point list. A tap goes through as touchStart/touchEnd
   * *and* as a mouse click, because a page's field focus follows the click that
   * Chrome synthesises from a tap, and that synthesis does not always happen for
   * injected touches.
   */
  const activeTouches = useRef<Map<number, { x: number; y: number }>>(new Map());
  /** Where the first touch went down, to tell a tap from a drag. */
  const touchStartAt = useRef<{ x: number; y: number } | null>(null);
  const touchMoved = useRef(false);
  /** Set briefly after a touch, so the mouse events the browser synthesises from
   *  it are ignored rather than forwarded a second time. */
  const suppressMouseAfterTouch = useRef(false);

  const pointsOf = useCallback(() => {
    const img = imgRef.current;
    const pts: Array<{ x: number; y: number }> = [];
    if (!img) return pts;
    const rect = img.getBoundingClientRect();
    for (const p of activeTouches.current.values()) {
      pts.push(toFrameCoords(p.x, p.y, rect, frame?.width ?? 0, frame?.height ?? 0));
    }
    return pts;
  }, [frame?.width, frame?.height]);

  const onTouchStart = useCallback(
    (e: React.TouchEvent) => {
      if (!isLive) return;
      // No preventDefault: React's touch handlers are passive, and the stage
      // already sets `touch-action: none`, which is what actually stops the
      // browser from scrolling it. Calling it here only logs a warning.
      suppressMouseAfterTouch.current = true;
      for (const t of Array.from(e.changedTouches)) {
        activeTouches.current.set(t.identifier, { x: t.clientX, y: t.clientY });
      }
      if (touchStartAt.current === null) {
        const first = e.changedTouches[0];
        if (first) touchStartAt.current = { x: first.clientX, y: first.clientY };
      }
      touchMoved.current = false;
      send(touchMessage('touchStart', pointsOf(), modifiersFromEvent(e)));
      // Focus the stage so a hardware keyboard also works, and so the page's own
      // on-screen keyboard has something to attach to.
      stageRef.current?.focus();
    },
    [isLive, pointsOf, send],
  );

  const onTouchMove = useCallback(
    (e: React.TouchEvent) => {
      if (!isLive) return;
      for (const t of Array.from(e.changedTouches)) {
        activeTouches.current.set(t.identifier, { x: t.clientX, y: t.clientY });
      }
      // A finger that travels more than a few pixels is a drag, not a tap, so the
      // synthetic click at the end must not be sent.
      const start = touchStartAt.current;
      const first = e.changedTouches[0];
      if (start && first) {
        const dx = first.clientX - start.x;
        const dy = first.clientY - start.y;
        if (dx * dx + dy * dy > 64) touchMoved.current = true;
      }
      send(touchMessage('touchMove', pointsOf(), modifiersFromEvent(e)));
    },
    [isLive, pointsOf, send],
  );

  const onTouchEnd = useCallback(
    (e: React.TouchEvent) => {
      if (!isLive) return;
      const wasTap = !touchMoved.current;
      const start = touchStartAt.current;
      send(touchMessage('touchEnd', pointsOf(), modifiersFromEvent(e)));
      for (const t of Array.from(e.changedTouches)) {
        activeTouches.current.delete(t.identifier);
      }
      if (activeTouches.current.size === 0) {
        touchStartAt.current = null;
        // A tap: also send the click Chrome would have made, so a field focuses
        // and the on-screen keyboard opens.
        if (wasTap && start) {
          const { x, y } = frameCoords(start.x, start.y);
          send(mouseMessage('mouseMoved', x, y, 'none', 0, 0, 0));
          send(mouseMessage('mousePressed', x, y, 'left', 1, 1, 0));
          send(mouseMessage('mouseReleased', x, y, 'left', 1, 0, 0));
        }
        // The browser fires its synthetic mouse events just after the touch
        // sequence; ignore them for a moment so the press is not sent twice.
        window.setTimeout(() => {
          suppressMouseAfterTouch.current = false;
        }, 400);
      }
    },
    [isLive, frameCoords, pointsOf, send],
  );

  const onTouchCancel = useCallback(
    (e: React.TouchEvent) => {
      if (!isLive) return;
      send(touchMessage('touchCancel', pointsOf(), modifiersFromEvent(e)));
      activeTouches.current.clear();
      touchStartAt.current = null;
      touchMoved.current = false;
    },
    [isLive, pointsOf, send],
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
            onTouchStart={onTouchStart}
            onTouchMove={onTouchMove}
            onTouchEnd={onTouchEnd}
            onTouchCancel={onTouchCancel}
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
