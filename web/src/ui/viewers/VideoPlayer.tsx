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
  IconPip, IconFullscreen, IconExitFullscreen, IconRotate,
} from '../primitives/Icons';
import { useMediaElement, RATES } from './useMediaElement';
import { formatClock } from './format';

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
            onClick={togglePlayback}
          />

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
