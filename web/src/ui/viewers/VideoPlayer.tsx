/**
 * VideoPlayer — full-page video with a custom control bar.
 *
 * The native controls are hidden so the bar matches the rest of the app, but
 * every affordance the native bar offers is reimplemented: play/pause, seek
 * with a buffered range, skip, volume, speed, picture-in-picture, fullscreen and
 * rotation. Keyboard shortcuts follow the conventions people already know.
 */

import { useCallback, useEffect, useRef, useState, type CSSProperties } from 'react';
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

export function VideoPlayer({ src, poster, onError }: VideoPlayerProps) {
  const [state, controls] = useMediaElement();
  const wrapRef = useRef<HTMLDivElement>(null);
  const [fullscreen, setFullscreen] = useState(false);
  const [showRate, setShowRate] = useState(false);
  /** 0 / 90 / 180 / 270 degrees, clockwise. */
  const [rotation, setRotation] = useState(0);
  /**
   * The stage's own size, so a quarter turn can size the video to the box's
   * transpose. Rotating in place would otherwise clip a landscape video to the
   * height of a portrait box (or the reverse).
   */
  const [box, setBox] = useState<{ w: number; h: number } | null>(null);

  useEffect(() => {
    if (state.error) onError?.(state.error);
  }, [state.error, onError]);

  // Track the stage size; a quarter turn swaps the axes it is drawn in.
  useEffect(() => {
    const el = wrapRef.current;
    if (!el || typeof ResizeObserver === 'undefined') return;
    const ro = new ResizeObserver(([entry]) => {
      const r = entry?.contentRect;
      if (r) setBox({ w: r.width, h: r.height });
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

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

  const rotate = useCallback(() => setRotation((r) => (r + 90) % 360), []);

  // Space toggles, arrows seek and adjust volume, F fullscreen, M mute, R rotates.
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
          rotate();
          break;
        default:
          break;
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [controls, state.volume, toggleFullscreen, rotate]);

  useEffect(() => {
    const onChange = () => setFullscreen(Boolean(document.fullscreenElement));
    document.addEventListener('fullscreenchange', onChange);
    return () => document.removeEventListener('fullscreenchange', onChange);
  }, []);

  const togglePip = useCallback(async () => {
    const el = wrapRef.current?.querySelector('video');
    if (!el) return;
    try {
      if (document.pictureInPictureElement) await document.exitPictureInPicture();
      else await el.requestPictureInPicture();
    } catch {
      // PiP is unsupported or blocked; nothing to do.
    }
  }, []);

  const pct = state.duration > 0 ? (state.currentTime / state.duration) * 100 : 0;
  const bufPct = state.duration > 0 ? (state.buffered / state.duration) * 100 : 0;
  // A quarter turn draws the video in the box's transpose, so a landscape clip
  // is not clipped to a portrait box's height (or the reverse).
  const quarterTurn = rotation % 180 !== 0;
  const rotatedStyle: CSSProperties = { transform: `rotate(${rotation}deg)` };
  if (quarterTurn && box) {
    rotatedStyle.width = `${box.h}px`;
    rotatedStyle.height = `${box.w}px`;
  }

  return (
    <>
      <div className={rotation === 0 ? 'vplayer' : 'vplayer is-rotated'} ref={wrapRef}>
        <video
          ref={controls.bind}
          className="vplayer__el"
          src={src}
          poster={poster}
          playsInline
          style={rotatedStyle}
          onClick={controls.toggle}
        />

        {state.error && (
          <div className="vplayer__error">
            <strong>{state.error}</strong>
            <span>Use “Open with” to play it in a media app.</span>
          </div>
        )}

        <div className="vplayer__bar">
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
              onClick={rotate}
              aria-label="Rotate 90 degrees"
              title={rotation === 0 ? 'Rotate' : `Rotated ${rotation}°`}
            >
              <IconRotate />
            </Button>
            <Button variant="ghost" size="sm" onClick={() => void togglePip()} aria-label="Picture in picture">
              <IconPip />
            </Button>
            <Button variant="ghost" size="sm" onClick={() => void toggleFullscreen()} aria-label="Fullscreen">
              {fullscreen ? <IconExitFullscreen /> : <IconFullscreen />}
            </Button>
          </div>
        </div>
      </div>
    </>
  );
}
