/**
 * VideoPlayer — full-page video with a custom control bar.
 *
 * The native controls are hidden so the bar matches the rest of the app, but
 * every affordance the native bar offers is reimplemented: play/pause, seek
 * with a buffered range, skip, volume, speed, orientation, picture-in-picture
 * and fullscreen. Keyboard shortcuts follow the conventions people already know.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { Button } from '../primitives/Button';
import {
  IconPlay, IconPause, IconVolume, IconVolumeOff, IconSkipBack, IconSkipForward,
  IconPip, IconFullscreen, IconExitFullscreen, IconRotate, IconSun,
} from '../primitives/Icons';
import { useMediaElement, RATES } from './useMediaElement';
import { formatClock } from './format';

/** How far a full-height drag moves the value. A little over 1 so the whole
 *  range is reachable without a perfect edge-to-edge swipe. */
const SWIPE_RANGE = 1.4;
/** Movement before a press becomes a swipe rather than a tap. */
const SWIPE_THRESHOLD_PX = 12;
/** Brightness range: down to a fifth, up to double, 1 being the source. */
const BRIGHTNESS_MIN = 0.2;
const BRIGHTNESS_MAX = 2;
/** How long the level indicator stays up after the last movement. */
const HUD_HIDE_MS = 900;

export interface VideoPlayerProps {
  src: string;
  poster?: string;
  onError?: (message: string) => void;
}

/**
 * How — or whether — this browser can float the video in a small window.
 *
 * There is no single API. Chromium and Firefox expose `requestPictureInPicture`
 * and gate it behind `document.pictureInPictureEnabled`; Safari uses
 * `webkitSetPresentationMode`. Mobile Chrome has none of it, so the control is
 * hidden there rather than shown as a button that does nothing.
 */
export type PipSupport = 'standard' | 'webkit' | 'none';

type WebkitVideo = HTMLVideoElement & {
  webkitSupportsPresentationMode?: (mode: string) => boolean;
  webkitSetPresentationMode?: (mode: string) => void;
  webkitPresentationMode?: string;
};

/** Which PiP API the element supports, if any. */
export function detectPipSupport(el: HTMLVideoElement | null): PipSupport {
  if (!el) return 'none';
  if (typeof document !== 'undefined' && document.pictureInPictureEnabled) {
    if (typeof el.requestPictureInPicture === 'function') return 'standard';
  }
  const wk = el as WebkitVideo;
  try {
    if (
      typeof wk.webkitSupportsPresentationMode === 'function' &&
      wk.webkitSupportsPresentationMode('picture-in-picture')
    ) {
      return 'webkit';
    }
  } catch {
    // Some builds expose the method but throw for this mode.
  }
  return 'none';
}

/**
 * How the video sits in the window:
 *   portrait  — upright, fitted to the window (the default)
 *   landscape — turned on its side, so a phone held upright shows the video the
 *               way a desktop screen does
 *
 * Landscape is done entirely in CSS: the wrapper is given the window's
 * *transpose* (height/width swapped) and turned a quarter turn about its own
 * centre, which is the standard technique used by web players for this. Nothing
 * is measured in JS, so there is no feedback loop and no dependence on the
 * element's current size. The picture stays `object-fit: contain` in both
 * modes — the video is never cropped.
 */
type Orientation = 'portrait' | 'landscape';

export function VideoPlayer({ src, poster, onError }: VideoPlayerProps) {
  const [state, controls] = useMediaElement();
  const wrapRef = useRef<HTMLDivElement>(null);
  const [fullscreen, setFullscreen] = useState(false);
  const [showRate, setShowRate] = useState(false);
  const [orientation, setOrientation] = useState<Orientation>('portrait');
  /**
   * Whether the control bar is showing. It hides itself after a few seconds of
   * playback so it does not sit over the picture, and a tap brings it back —
   * the behaviour every player has. It never hides while paused, because a
   * paused video is exactly when the controls are wanted.
   */
  const [controlsVisible, setControlsVisible] = useState(true);
  const hideTimer = useRef<number | null>(null);
  /** Which PiP API this browser has, and whether we are currently in it. */
  const [pipSupport, setPipSupport] = useState<PipSupport>('none');
  const [inPip, setInPip] = useState(false);
  /**
   * Picture brightness, 1 being the source. Held in a filter rather than on the
   * element so it applies to the picture only, not to the controls drawn over it.
   */
  const [brightness, setBrightness] = useState(1);
  const brightnessRef = useRef(brightness);
  brightnessRef.current = brightness;

  // Detect PiP once the element exists: the capability lives on the element and
  // on `document`, not on the component.
  useEffect(() => {
    const el = wrapRef.current?.querySelector('video') ?? null;
    setPipSupport(detectPipSupport(el));
  }, []);

  const clearHideTimer = useCallback(() => {
    if (hideTimer.current !== null) {
      window.clearTimeout(hideTimer.current);
      hideTimer.current = null;
    }
  }, []);

  /** Show the bar and, if playing, schedule it to fade again. */
  const revealControls = useCallback(() => {
    setControlsVisible(true);
    clearHideTimer();
    if (state.playing) {
      hideTimer.current = window.setTimeout(() => setControlsVisible(false), 3000);
    }
  }, [clearHideTimer, state.playing]);

  /**
   * A tap on the picture. While the bar is hidden the tap only brings it back —
   * a tap that both revealed the controls and paused the video would be two
   * actions from one gesture, and would pause on the way to the pause button.
   */
  const togglePlayback = useCallback(() => {
    if (!controlsVisible) {
      revealControls();
      return;
    }
    controls.toggle();
    revealControls();
  }, [controlsVisible, revealControls, controls]);

  // Keep the timer in step with playback: playing hides after a pause, pausing
  // brings the bar back for good.
  useEffect(() => {
    clearHideTimer();
    if (state.playing) {
      hideTimer.current = window.setTimeout(() => setControlsVisible(false), 3000);
    } else {
      setControlsVisible(true);
    }
    return clearHideTimer;
  }, [state.playing, clearHideTimer]);

  useEffect(() => {
    if (state.error) onError?.(state.error);
  }, [state.error, onError]);

  const toggleFullscreen = useCallback(async () => {
    const el = wrapRef.current;
    if (!el) return;
    if (document.fullscreenElement) {
      await document.exitFullscreen().catch(() => undefined);
      setFullscreen(false);
    } else {
      await el.requestFullscreen().catch(() => undefined);
      setFullscreen(true);
    }
  }, []);

  const toggleOrientation = useCallback(
    () => setOrientation((o) => (o === 'portrait' ? 'landscape' : 'portrait')),
    [],
  );

  // Space toggles, arrows seek and adjust volume, F fullscreen, M mute,
  // R turns the video (the key players use for orientation).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
      switch (e.key) {
        case ' ':
        case 'k':
          e.preventDefault();
          controls.toggle();
          break;
        case 'ArrowRight':
          controls.seekBy(5);
          break;
        case 'ArrowLeft':
          controls.seekBy(-5);
          break;
        case 'ArrowUp':
          controls.setVolume(state.volume + 0.05);
          break;
        case 'ArrowDown':
          controls.setVolume(state.volume - 0.05);
          break;
        case 'm':
          controls.toggleMute();
          break;
        case 'f':
          void toggleFullscreen();
          break;
        case 'r':
          toggleOrientation();
          break;
        default:
          break;
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [controls, state.volume, toggleFullscreen, toggleOrientation]);

  useEffect(() => {
    const onChange = () => setFullscreen(Boolean(document.fullscreenElement));
    document.addEventListener('fullscreenchange', onChange);
    return () => document.removeEventListener('fullscreenchange', onChange);
  }, []);

  /**
   * Float the video in a small window, or bring it back.
   *
   * Two APIs, and a real failure path. The standard one is Chromium/Firefox; the
   * `webkitSetPresentationMode` one is Safari. A rejection is reported rather than
   * swallowed, because a control that silently does nothing is worse than one
   * that says why.
   */
  const togglePip = useCallback(async () => {
    const el = wrapRef.current?.querySelector('video') as WebkitVideo | null;
    if (!el) return;

    if (pipSupport === 'standard') {
      try {
        if (document.pictureInPictureElement) await document.exitPictureInPicture();
        else await el.requestPictureInPicture();
        setInPip(Boolean(document.pictureInPictureElement));
      } catch (e) {
        onError?.(e instanceof Error ? e.message : 'Picture in picture was refused.');
      }
      return;
    }

    if (pipSupport === 'webkit') {
      try {
        const next = el.webkitPresentationMode === 'picture-in-picture' ? 'inline' : 'picture-in-picture';
        el.webkitSetPresentationMode?.(next);
        setInPip(next === 'picture-in-picture');
      } catch {
        onError?.('Picture in picture was refused.');
      }
    }
  }, [pipSupport, onError]);

  // Track the browser's own state, so the button reflects reality even when the
  // user leaves PiP with the window's close button rather than ours.
  useEffect(() => {
    const el = wrapRef.current?.querySelector('video') as WebkitVideo | null;
    if (!el) return;
    const onEnter = () => setInPip(true);
    const onLeave = () => setInPip(false);
    const onWebkitChange = () =>
      setInPip(el.webkitPresentationMode === 'picture-in-picture');
    el.addEventListener('enterpictureinpicture', onEnter);
    el.addEventListener('leavepictureinpicture', onLeave);
    el.addEventListener('webkitpresentationmodechanged', onWebkitChange);
    return () => {
      el.removeEventListener('enterpictureinpicture', onEnter);
      el.removeEventListener('leavepictureinpicture', onLeave);
      el.removeEventListener('webkitpresentationmodechanged', onWebkitChange);
    };
  }, [pipSupport]);

  /**
   * The level indicator for a swipe gesture.
   *
   * `side` is which half of the picture the swipe started on — the left sets
   * brightness, the right sets volume — and it is null when no indicator is
   * showing. It hides itself a moment after the last movement, so it behaves
   * like the control bar rather than a permanent overlay.
   */
  const [hud, setHud] = useState<{ side: 'brightness' | 'volume'; value: number } | null>(null);
  const hudTimer = useRef<number | null>(null);
  /**
   * The gesture in progress. A press only becomes a swipe once it has moved
   * past the threshold, so a tap still toggles playback.
   */
  const swipe = useRef<{
    side: 'brightness' | 'volume';
    startY: number;
    startValue: number;
    active: boolean;
  } | null>(null);

  const clearHudTimer = useCallback(() => {
    if (hudTimer.current !== null) {
      window.clearTimeout(hudTimer.current);
      hudTimer.current = null;
    }
  }, []);

  useEffect(() => clearHudTimer, [clearHudTimer]);

  /** Show the indicator and schedule it to fade, restarting the clock each time. */
  const showHud = useCallback(
    (side: 'brightness' | 'volume', value: number) => {
      setHud({ side, value });
      clearHudTimer();
      hudTimer.current = window.setTimeout(() => setHud(null), HUD_HIDE_MS);
    },
    [clearHudTimer],
  );

  const clampBrightness = (v: number) =>
    Math.min(BRIGHTNESS_MAX, Math.max(BRIGHTNESS_MIN, v));

  /**
   * A press on the picture. Which half decides what it will control; the
   * gesture only takes effect once it moves, so a tap still plays or pauses.
   */
  const onGestureDown = useCallback(
    (e: React.PointerEvent) => {
      // Measured against the frame, not the video: in landscape the video is
      // rotated, so its own box is the window's transpose and would put the
      // sides and the span in the wrong places.
      const r = wrapRef.current?.getBoundingClientRect();
      if (!r) return;
      const onLeft = e.clientX - r.left < r.width / 2;
      const side = onLeft ? 'brightness' : 'volume';
      swipe.current = {
        side,
        startY: e.clientY,
        startValue: side === 'brightness' ? brightnessRef.current : state.muted ? 0 : state.volume,
        active: false,
      };
      try {
        (e.currentTarget as Element).setPointerCapture(e.pointerId);
      } catch {
        /* no active pointer; the gesture still ends on release */
      }
    },
    [state.muted, state.volume],
  );

  const onGestureMove = useCallback(
    (e: React.PointerEvent) => {
      const g = swipe.current;
      if (!g) return;
      // Up increases, which is the direction everyone expects; a swipe is
      // measured against the height of the picture so it scales with the window.
      const dy = g.startY - e.clientY;
      if (!g.active && Math.abs(dy) < SWIPE_THRESHOLD_PX) return;
      g.active = true;

      const box = wrapRef.current?.getBoundingClientRect();
      const span = Math.max(1, box?.height ?? window.innerHeight) * SWIPE_RANGE;
      const delta = dy / span;

      if (g.side === 'brightness') {
        const next = clampBrightness(g.startValue + delta);
        setBrightness(next);
        showHud('brightness', next);
      } else {
        const next = Math.min(1, Math.max(0, g.startValue + delta));
        controls.setVolume(next);
        showHud('volume', next);
      }
    },
    [controls, showHud],
  );

  const onGestureUp = useCallback(
    (e: React.PointerEvent) => {
      try {
        (e.currentTarget as Element).releasePointerCapture?.(e.pointerId);
      } catch {
        /* already released */
      }
      const g = swipe.current;
      swipe.current = null;
      // A press that never moved is a tap: play or pause, as before.
      if (g && !g.active) togglePlayback();
    },
    [togglePlayback],
  );

  const pct = state.duration > 0 ? (state.currentTime / state.duration) * 100 : 0;
  const bufPct = state.duration > 0 ? (state.buffered / state.duration) * 100 : 0;
  const landscape = orientation === 'landscape';

  return (
    <>
      <div
        className={landscape ? 'vplayer__frame is-landscape' : 'vplayer__frame'}
        ref={wrapRef}
      >
        <div
          className={landscape ? 'vplayer is-landscape' : 'vplayer'}
          // Desktop: moving the mouse over the picture brings the controls back,
          // the way every player behaves. Touch uses the tap instead.
          onMouseMove={revealControls}
        >
          <video
            ref={controls.bind}
            className={
              controlsVisible ? 'vplayer__el' : 'vplayer__el is-hidden-cursor'
            }
            src={src}
            poster={poster}
            playsInline
            style={brightness !== 1 ? { filter: `brightness(${brightness})` } : undefined}
            // A vertical swipe sets brightness on the left half and volume on
            // the right. A press that does not move is still a tap, so the
            // pointer handlers replace the click rather than adding to it.
            onPointerDown={onGestureDown}
            onPointerMove={onGestureMove}
            onPointerUp={onGestureUp}
            onPointerCancel={onGestureUp}
          />

          {hud && (
            <div
              className={`vplayer__hud vplayer__hud--${hud.side}`}
              role="status"
              aria-label={`${hud.side === 'brightness' ? 'Brightness' : 'Volume'} ${Math.round(
                hud.value * 100,
              )}%`}
            >
              <div className="vplayer__hud-track">
                <div
                  className="vplayer__hud-fill"
                  style={{ height: `${Math.round(hud.value * 100)}%` }}
                />
              </div>
              <span className="vplayer__hud-icon">
                {hud.side === 'brightness' ? (
                  <IconSun />
                ) : hud.value === 0 ? (
                  <IconVolumeOff />
                ) : (
                  <IconVolume />
                )}
              </span>
              <span className="vplayer__hud-value">{Math.round(hud.value * 100)}%</span>
            </div>
          )}

          {state.error && (
            <div className="vplayer__error">
              <strong>{state.error}</strong>
              <span>Use “Open with” to play it in a media app.</span>
            </div>
          )}

          <div
            className={controlsVisible ? 'vplayer__bar' : 'vplayer__bar is-hidden'}
            aria-hidden={!controlsVisible}
            // Any interaction with the bar keeps it up while the user is using it.
            onPointerDown={revealControls}
            onMouseMove={revealControls}
          >
            <button
              type="button"
              className="vplayer__scrub"
              aria-label="Seek"
              onClick={(e) => {
                const r = e.currentTarget.getBoundingClientRect();
                controls.seek(((e.clientX - r.left) / r.width) * state.duration);
              }}
            >
              <span className="vplayer__buffered" style={{ width: `${bufPct}%` }} />
              <span className="vplayer__played" style={{ width: `${pct}%` }} />
              <span className="vplayer__knob" style={{ left: `${pct}%` }} />
            </button>

            <div className="vplayer__row">
              <Button variant="ghost" size="sm" onClick={() => controls.seekBy(-10)} aria-label="Back 10 seconds">
                <IconSkipBack />
              </Button>
              <Button variant="ghost" size="sm" onClick={controls.toggle} aria-label={state.playing ? 'Pause' : 'Play'}>
                {state.playing ? <IconPause /> : <IconPlay />}
              </Button>
              <Button variant="ghost" size="sm" onClick={() => controls.seekBy(10)} aria-label="Forward 10 seconds">
                <IconSkipForward />
              </Button>

              <span className="vplayer__time">
                {formatClock(state.currentTime)} / {formatClock(state.duration)}
              </span>

              <span className="vplayer__spacer" />

              <Button variant="ghost" size="sm" onClick={controls.toggleMute} aria-label={state.muted ? 'Unmute' : 'Mute'}>
                {state.muted || state.volume === 0 ? <IconVolumeOff /> : <IconVolume />}
              </Button>
              <input
                className="vplayer__volume"
                type="range"
                min={0}
                max={1}
                step={0.05}
                value={state.muted ? 0 : state.volume}
                aria-label="Volume"
                onChange={(e) => controls.setVolume(Number(e.target.value))}
              />

              <div className="vplayer__rate">
                <Button variant="ghost" size="sm" onClick={() => setShowRate((v) => !v)}>
                  {state.rate}×
                </Button>
                {showRate && (
                  <div className="vplayer__rate-list">
                    {RATES.map((r) => (
                      <button
                        key={r}
                        type="button"
                        className={r === state.rate ? 'vplayer__rate-item is-active' : 'vplayer__rate-item'}
                        onClick={() => {
                          controls.setRate(r);
                          setShowRate(false);
                        }}
                      >
                        {r}×
                      </button>
                    ))}
                  </div>
                )}
              </div>

              <Button
                variant="ghost"
                size="sm"
                onClick={toggleOrientation}
                aria-label={landscape ? 'Upright' : 'Turn sideways'}
                aria-pressed={landscape}
                title={landscape ? 'Upright' : 'Sideways — like a desktop screen'}
              >
                {/* Always the rotate glyph. The fullscreen icon is the same
                    four-corner shape, so swapping to it read as a duplicate
                    control rather than a state. */}
                <IconRotate />
              </Button>
              {/* Only offered where the browser can actually do it. Mobile Chrome
                  has no PiP API, so a button there would do nothing. */}
              {pipSupport !== 'none' && (
                <Button
                  variant="ghost"
                  size="sm"
                  onClick={() => void togglePip()}
                  aria-label={inPip ? 'Exit picture in picture' : 'Picture in picture'}
                  aria-pressed={inPip}
                  title={inPip ? 'Exit picture in picture' : 'Picture in picture'}
                >
                  <IconPip />
                </Button>
              )}
              <Button variant="ghost" size="sm" onClick={() => void toggleFullscreen()} aria-label="Fullscreen">
                {fullscreen ? <IconExitFullscreen /> : <IconFullscreen />}
              </Button>
            </div>
          </div>
        </div>
      </div>
    </>
  );
}
