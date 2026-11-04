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

import { useCallback, useEffect, useRef, useState } from 'react';
import { Button } from '../primitives/Button';
import { Spinner } from '../primitives/Spinner';
import { Note } from '../primitives/Note';
import { EmptyState } from '../primitives/EmptyState';
import {
  IconRefresh,
  IconExternal,
  IconX,
  IconImage,
  IconDownload,
  IconArrowLeft,
  IconArrowRight,
} from '../primitives/Icons';
import { MediaHud } from './MediaHud';
import type { LiveMediaItem, LiveMediaVariant } from '../../hooks';
import type { LiveFailure, LiveFrame, LivePageState, LiveStreamStats } from '../../hooks';

import {
  keyMessage,
  modifiersFromEvent,
  mouseMessage,
  resizeMessage,
  textMessage,
  toFrameCoords,
  touchMessage,
  wheelMessage,
} from './live-input';

export type LiveStatus = 'idle' | 'connecting' | 'open' | 'closed';

export interface LiveViewProps {
  status: LiveStatus;
  frame: LiveFrame | null;
  page: LivePageState | null;
  /** The size the engine is drawing at, once open. Locks the stage's shape. */
  viewport: { width: number; height: number } | null;
  /**
   * What the stream is doing: frames received, frames skipped, worst gap.
   *
   * Shown in the bar because a live view that stutters is otherwise impossible
   * to diagnose from the outside — a gap means the engine dropped frames to keep
   * up, and a large gap with no skips means the link itself is slow.
   */
  stats: LiveStreamStats | null;
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
  /**
   * A raw-video session, when the engine can host one and the viewer chose
   * it. Absent means the JPEG path is the only one available, which is the
   * behaviour this view had before video existed.
   *
   * The element is created here and handed to the session, because a
   * MediaStream can only be attached to an element that is already in the
   * DOM — so the session cannot own it.
   */
  video?: {
    status: 'idle' | 'connecting' | 'live' | 'ended';
    viewport: { width: number; height: number } | null;
    reason: string | null;
    error: string | null;
    open: (url: string, video: HTMLVideoElement) => void;
    close: () => void;
  };
  /** Whether to show the video session rather than the JPEG one. */
  useVideo?: boolean;
  /**
   * The media the page has loaded, and the controls for collecting it.
   *
   * Optional so a caller that has no download facility — a test, a preview —
   * still renders the page; the HUD button is simply not offered.
   */
  media?: {
    items: LiveMediaItem[];
    watching: boolean;
    watch: (on: boolean) => void;
    forget: (url: string) => void;
    rescan: () => void;
  };
  /** Queue a download for a URL. `quality` is the variant label, when one was
   *  chosen from a manifest. */
  onDownload?: (url: string, quality?: string) => void;
  /** URLs already queued, so a row can say so. */
  queued?: ReadonlySet<string>;
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

export function InteractiveView({
  url,
  live,
  video,
  useVideo,
  media,
  onDownload,
  queued,
}: InteractiveViewProps) {
  const { status, frame, page, viewport, stats, failure, reason, onOpen, onClose, send } = live;
  /* Video replaces the picture when it is chosen and a session is possible.
   * Everything else — the input bridge, the sizing, the toolbar — is shared,
   * so only the stage's contents differ. */
  const videoOn = Boolean(useVideo && video);
  const videoLive = videoOn && video?.status === 'live';

  /**
   * Open the video session, once the element it draws into exists.
   *
   * This is why the view opens it rather than the caller: a MediaStream can
   * only be attached to an element already in the DOM, and the element is
   * created here. `open` replaces any session already running, so a change of
   * address or a re-mount is handled without a teardown dance.
   */
  const videoUrl = url.trim();
  const videoOpen = video?.open;
  const videoClose = video?.close;
  useEffect(() => {
    if (!videoOn || !videoUrl || !videoOpen) return;
    const el = videoRef.current;
    if (!el) return;
    videoOpen(videoUrl, el);
  }, [videoOn, videoUrl, videoOpen]);

  // A session left running when the stage goes away would hold the engine's one
  // slot, so it is closed on unmount.
  useEffect(
    () => () => {
      videoClose?.();
    },
    [videoClose],
  );
  const stageRef = useRef<HTMLDivElement | null>(null);
  const imgRef = useRef<HTMLImageElement | null>(null);
  /**
   * The <video> the WebRTC stream is drawn into, when one is playing.
   *
   * It stands in for the <img> as the thing input is mapped against: the
   * element's box is the page's box either way, so touch, swipe and wheel
   * keep working unchanged whichever is showing.
   */
  const videoRef = useRef<HTMLVideoElement | null>(null);
  /**
   * The element that actually takes the keyboard.
   *
   * A `<div>` cannot raise the on-screen keyboard on a phone, however it is
   * focused — only an editable element can. This hidden textarea is kept
   * focusable and empty: a tap focuses it (which opens the keyboard), and every
   * character that arrives is forwarded to the page and then cleared.
   */
  const kbdRef = useRef<HTMLTextAreaElement | null>(null);
  /** The last viewport we told the engine about, so we do not repeat ourselves. */
  const sentSize = useRef<{ w: number; h: number }>({ w: 0, h: 0 });
  /*
   * Whether the stage is driving a page, which is what enables the input
   * bridge and the sizing. Either transport counts: the video session does
   * not set the JPEG status, so deriving this from that alone left the video
   * stage inert — no input, no sizing, and nothing to see.
   */
  const isLive = status === 'open' || status === 'connecting' || videoOn;
  /** Whether the media overlay is showing. Off until asked for. */
  const [hudOpen, setHudOpen] = useState(false);

  /**
   * Tell the engine the size the page should be drawn at.
   *
   * Only used when the viewport is *not* fixed — on a phone it is, so this is
   * inert there. Re-fitting a live page on every stage resize is what made the
   * page reflow under the user's finger as the URL bar and keyboard moved.
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

  /*
   * Keep the browser's viewport in step with the stage — but only on a desktop.
   *
   * A fixed viewport is the whole point of the mobile behaviour: the page is
   * drawn at the phone's shape and the stage is fitted to *it*, rather than the
   * page being re-fitted to a stage that keeps changing size.
   */
  const fixedViewport = viewport !== null;
  useEffect(() => {
    if (fixedViewport) return;
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
  }, [fixedViewport, isLive, pushSize]);

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
      const el = videoRef.current ?? imgRef.current;
      if (!el) return;
      const { x, y } = toFrameCoords(
        e.clientX,
        e.clientY,
        el.getBoundingClientRect(),
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
      const el = videoRef.current ?? imgRef.current;
      if (!el) return { x: 0, y: 0 };
      return toFrameCoords(
        clientX,
        clientY,
        el.getBoundingClientRect(),
        frame?.width ?? 0,
        frame?.height ?? 0,
      );
    },
    [frame?.width, frame?.height],
  );

  /*
   * Raise the on-screen keyboard only when the page is ready for text.
   *
   * A tap focuses whatever is under it — a link, a button, blank space — and a
   * real browser only opens a keyboard when that was a text field. The console
   * cannot see the page's DOM, so the engine reports whether an editable element
   * is focused (`page.editing`) and the keyboard is raised on that answer rather
   * than on the tap. A tap that focuses nothing therefore does nothing.
   */
  const editing = page?.editing ?? false;
  useEffect(() => {
    if (!isLive) return;
    if (editing) kbdRef.current?.focus();
  }, [editing, isLive]);

  const onMouseDown = useCallback(
    (e: React.MouseEvent) => {
      if (!isLive) return;
      /*
       * A press on a non-focusable element blurs whatever had focus, so the
       * default is prevented to keep the caret where the page put it. The
       * keyboard itself is raised from the page's own answer (`page.editing`),
       * not from the press, so a press on a link does not open one.
       */
      e.preventDefault();
      /*
       * On a touch device the browser also synthesises mouse events from a tap,
       * after the touch handlers have already forwarded the tap and its click. Let
       * those through and the page receives every press twice — a double click,
       * which selects a word instead of placing a caret.
       */
      if (suppressMouseAfterTouch.current) return;
      const { x, y } = frameCoords(e.clientX, e.clientY);
      send(mouseMessage('mousePressed', x, y, buttonName(e.button), 1, e.buttons, modifiersFromEvent(e)));
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

  /*
   * The keyboard.
   *
   * Keys are read from a hidden textarea, not the stage, because that is the only
   * element a phone will raise a keyboard for — a `<div>` cannot, however it is
   * focused. Two paths, and the split matters:
   *
   *   - `keydown` gives the *named* keys: Backspace, Enter, the arrows, and any
   *     shortcut. Forwarded as key events.
   *   - `input` gives committed text, which is what an on-screen keyboard, an IME
   *     or a paste produces. Forwarded as text, because a phone's keyboard does
   *     not always send a usable `key`/`code` for a character.
   *
   * These are *native* listeners rather than React props on purpose. React's
   * `onBeforeInput` is a polyfill built on `keypress`/`textInput` and does not
   * fire reliably, which is how an earlier version of this shipped a keyboard
   * that silently sent nothing. The field is cleared after each event so it never
   * accumulates text, and `input` is diffed against what we already sent so a
   * composition that arrives in pieces is forwarded exactly once.
   */
  useEffect(() => {
    const el = kbdRef.current;
    if (!el || !isLive) return;

    /*
     * Composition state, which is what a phone's keyboard uses.
     *
     * On a touch device the keyboard does not send a key per character; it
     * composes a word and only commits it at the end. Clearing the field during a
     * composition discards the word before it is ever committed, which is why
     * typing appeared to do nothing. The field is left alone until the
     * composition ends.
     */
    let composing = false;

    const onNativeCompositionStart = () => {
      composing = true;
    };

    const onNativeCompositionEnd = () => {
      composing = false;
      // The committed text is in the field now; forward it and clear.
      const text = el.value;
      el.value = '';
      if (text) send(textMessage(text));
    };

    const onNativeKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        el.blur();
        return;
      }
      if (e.key === 'F5' || (e.ctrlKey && e.key === 'r')) return;
      // During a composition the field is being built; leave it alone.
      if (composing || e.isComposing) return;
      // A phone keyboard reports a character it cannot name as "Unidentified" or
      // "Process"; those are not keys to forward, and the character itself arrives
      // through `input`.
      if (e.key === 'Unidentified' || e.key === 'Process') return;
      // A printable character comes through `input` instead, so it is not sent
      // twice. Everything else is a named key.
      const printable = e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey;
      if (printable) return;
      e.preventDefault();
      send(
        keyMessage('keyDown', {
          key: e.key,
          code: e.code,
          keyCode: e.keyCode,
          altKey: e.altKey,
          ctrlKey: e.ctrlKey,
          metaKey: e.metaKey,
          shiftKey: e.shiftKey,
        }),
      );
    };

    const onNativeInput = () => {
      // Mid-composition the text is a draft: sending it now would type it twice,
      // once as a draft and once when it commits.
      if (composing) return;
      const text = el.value;
      // Clear immediately: the field is a keyboard, not a text box, and leaving
      // the value in place would let it grow without bound.
      el.value = '';
      if (text) send(textMessage(text));
    };

    const onNativePaste = (e: ClipboardEvent) => {
      const text = e.clipboardData?.getData('text') ?? '';
      if (text) {
        e.preventDefault();
        el.value = '';
        send(textMessage(text));
      }
    };

    el.addEventListener('compositionstart', onNativeCompositionStart);
    el.addEventListener('compositionend', onNativeCompositionEnd);
    el.addEventListener('keydown', onNativeKeyDown);
    el.addEventListener('input', onNativeInput);
    el.addEventListener('paste', onNativePaste);
    return () => {
      el.removeEventListener('compositionstart', onNativeCompositionStart);
      el.removeEventListener('compositionend', onNativeCompositionEnd);
      el.removeEventListener('keydown', onNativeKeyDown);
      el.removeEventListener('input', onNativeInput);
      el.removeEventListener('paste', onNativePaste);
    };
  }, [isLive, send]);

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
          {/*
            Navigation, as a browser's own controls. Back and Forward are
            disabled when there is nowhere to go rather than doing nothing, which
            is what a browser does and what makes the state legible.
          */}
          <Button
            size="sm"
            onClick={() => send({ type: 'back' })}
            disabled={!isLive}
            title="Back"
            aria-label="Back"
          >
            <IconArrowLeft />
          </Button>
          <Button
            size="sm"
            onClick={() => send({ type: 'forward' })}
            disabled={!isLive}
            title="Forward"
            aria-label="Forward"
          >
            <IconArrowRight />
          </Button>
          <Button size="sm" onClick={() => send({ type: 'reload' })} disabled={!isLive}>
            <IconRefresh /> Reload
          </Button>
          <Button
            size="sm"
            onClick={() => send({ type: 'stop' })}
            disabled={!isLive || !page?.loading}
            title="Stop loading"
          >
            <IconX /> Stop
          </Button>
          {media && onDownload ? (
            <Button
              size="sm"
              variant={hudOpen ? 'primary' : undefined}
              onClick={() => {
                // Opening the HUD is also what starts collecting: a viewer who
                // never opens it never pays for the bookkeeping.
                if (!hudOpen && !media.watching) media.watch(true);
                setHudOpen(!hudOpen);
              }}
              aria-pressed={hudOpen}
              title="Everything this page has loaded"
            >
              <IconDownload /> Media
              {media.items.length > 0 ? (
                <span className="live__badge">{media.items.length}</span>
              ) : null}
            </Button>
          ) : null}
          <Button
            size="sm"
            onClick={() => send({ type: 'screenshot' })}
            disabled={!isLive}
            title="Ask the engine for a full-resolution still"
          >
            <IconImage /> Still
          </Button>
          <Button size="sm" onClick={onClose} disabled={!isLive} title="End the session">
            <IconX /> Close
          </Button>
          <a className="btn btn--sm" href={liveUrl} target="_blank" rel="noreferrer">
            <IconExternal /> New tab
          </a>
        </div>
      </div>

      {/* The empty state is about the JPEG session; in video mode there is no
          JPEG session, so the stage below is the thing to show. */}
      {status === 'idle' && !videoOn && (
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
          {/*
            The keyboard target.
            
            A `<div>` cannot raise the on-screen keyboard on a phone, however it
            is focused; only an editable element can. This textarea is the
            element a tap focuses, so a phone opens its keyboard. It is kept
            empty and off to the side — it is a keyboard, not a text box, and
            every character it receives is forwarded to the page and cleared.

            `autoCapitalize`/`autoCorrect` are off because the page being driven
            is the one that should decide how text is entered, not this field.
          */}
          <textarea
            ref={kbdRef}
            className="live__kbd"
            aria-hidden="true"
            tabIndex={-1}
            autoCapitalize="off"
            autoCorrect="off"
            autoComplete="off"
            spellCheck={false}
          />
          <div
            className={fixedViewport ? 'live__stage live__stage--fixed' : 'live__stage'}
            ref={stageRef}
            role="application"
            aria-label={`Live page ${liveUrl}`}
            style={
              fixedViewport && viewport
                ? ({ '--live-ar': `${viewport.width} / ${viewport.height}` } as React.CSSProperties)
                : undefined
            }
            onMouseDown={onMouseDown}
            onMouseUp={onMouseUp}
            onMouseMove={onMouseMove}
            onTouchStart={onTouchStart}
            onTouchMove={onTouchMove}
            onTouchEnd={onTouchEnd}
            onTouchCancel={onTouchCancel}
          >
            {videoOn ? (
              <>
                <video
                  ref={videoRef}
                  className="live__frame"
                  autoPlay
                  playsInline
                  muted
                  aria-label={`Live video of ${liveUrl}`}
                />
                {!videoLive ? (
                  <div className="live__blank">
                    <Spinner />
                    <span>
                      {video?.status === 'ended'
                        ? (video.reason ?? 'The video session ended')
                        : 'Connecting the video stream…'}
                    </span>
                  </div>
                ) : null}
              </>
            ) : shownFrame ? (
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
            {/*
              The media overlay.

              Inside the stage, because it overlays *the page*: as a sibling it
              anchored to the page and the top bar sat over it. An overlay rather
              than a panel, because the page underneath is the point of the view —
              a listing that pushed it aside would make you choose between seeing
              the page and seeing what it offers. Dismissing it does not end the
              session.
            */}
            {hudOpen && media && onDownload ? (
              <MediaHud
                items={media.items}
                watching={media.watching}
                queued={queued}
                onWatch={media.watch}
                onRescan={media.rescan}
                onRemove={media.forget}
                onClose={() => setHudOpen(false)}
                onDownload={(item: LiveMediaItem, variant?: LiveMediaVariant) =>
                  // The variant's own URL when one was chosen, because that is the
                  // playlist for that quality; the manifest URL otherwise.
                  onDownload(variant?.url ?? item.url, variant?.label)
                }
              />
            ) : null}
          </div>
          <p className="live__hint">
            Tap or click the page to focus it, then type. Escape releases the keyboard. Scrolling is
            forwarded to the page.
          </p>
          {/*
            What the stream is doing, in the open. A live view that stutters is
            otherwise impossible to diagnose from the outside: a rising skip count
            means the engine is dropping frames to keep up, while a large worst-gap
            with no skips means the link itself is slow. Both are worth knowing,
            and neither is guessable from the picture.
          */}
          {stats && (
            <p className="live__stats" title="How the picture stream is performing">
              {stats.received} frames
              {stats.skipped > 0 ? ` · ${stats.skipped} skipped` : ''}
              {stats.lastArrivalMs > 0 ? ` · ${Math.round(stats.lastArrivalMs)}ms apart` : ''}
              {stats.worstGapMs > 0 ? ` · worst ${Math.round(stats.worstGapMs)}ms` : ''}
            </p>
          )}
        </>
      )}

      {reason && status === 'closed' && <Note className="note note--warn">{reason}</Note>}
    </div>
  );
}
