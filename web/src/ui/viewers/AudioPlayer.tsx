/**
 * AudioPlayer — full-page audio with artwork, transport and a waveform.
 *
 * The waveform is decorative: a browser gives no access to sample data here,
 * so drawing a fake one and calling it a visualiser would be dishonest. It is
 * marked `aria-hidden` and never pretends to show real amplitude — the seek
 * bar below it is the thing that carries real position.
 */

import { useEffect } from 'react';
import { Button } from '../primitives/Button';
import {
  IconPlay, IconPause, IconSkipBack, IconSkipForward, IconVolume, IconVolumeOff, IconMusic,
} from '../primitives/Icons';
import { useMediaElement, RATES } from './useMediaElement';
import { formatClock } from './format';

export interface AudioPlayerProps {
  src: string;
  title: string;
  onError?: (message: string) => void;
}

export function AudioPlayer({ src, title, onError }: AudioPlayerProps) {
  const [state, controls] = useMediaElement();

  useEffect(() => {
    if (state.error) onError?.(state.error);
  }, [state.error, onError]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === 'INPUT' || tag === 'SELECT') return;
      if (e.key === ' ' || e.key === 'k') {
        e.preventDefault();
        controls.toggle();
      } else if (e.key === 'ArrowRight') controls.seekBy(5);
      else if (e.key === 'ArrowLeft') controls.seekBy(-5);
      else if (e.key === 'm') controls.toggleMute();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [controls]);

  const pct = state.duration > 0 ? (state.currentTime / state.duration) * 100 : 0;

  return (
    <div className="aplayer">
      <audio ref={controls.bind} src={src} preload="metadata" />

      <div className="aplayer__art" aria-hidden="true">
        <IconMusic />
      </div>
      <div className="aplayer__title" title={title}>
        {title}
      </div>

      {/* Decorative only — no sample data is available to draw honestly. */}
      <div className="aplayer__wave" aria-hidden="true">
        {Array.from({ length: 56 }, (_, i) => (
          <span key={i} style={{ height: `${10 + ((i * 13) % 26)}px` }} />
        ))}
      </div>

      <button
        type="button"
        className="aplayer__scrub"
        aria-label="Seek"
        onClick={(e) => {
          const r = e.currentTarget.getBoundingClientRect();
          controls.seek(((e.clientX - r.left) / r.width) * state.duration);
        }}
      >
        <span className="aplayer__played" style={{ width: `${pct}%` }} />
        <span className="aplayer__knob" style={{ left: `${pct}%` }} />
      </button>
      <div className="aplayer__times">
        <span>{formatClock(state.currentTime)}</span>
        <span>{formatClock(state.duration)}</span>
      </div>

      <div className="aplayer__row">
        <Button variant="ghost" size="sm" onClick={() => controls.seekBy(-10)} aria-label="Back 10 seconds">
          <IconSkipBack />
        </Button>
        <Button variant="primary" onClick={controls.toggle} aria-label={state.playing ? 'Pause' : 'Play'}>
          {state.playing ? <IconPause /> : <IconPlay />}
        </Button>
        <Button variant="ghost" size="sm" onClick={() => controls.seekBy(10)} aria-label="Forward 10 seconds">
          <IconSkipForward />
        </Button>

        <span className="aplayer__spacer" />

        <Button variant="ghost" size="sm" onClick={controls.toggleMute} aria-label={state.muted ? 'Unmute' : 'Mute'}>
          {state.muted || state.volume === 0 ? <IconVolumeOff /> : <IconVolume />}
        </Button>
        <input
          className="aplayer__volume"
          type="range"
          min={0}
          max={1}
          step={0.05}
          value={state.muted ? 0 : state.volume}
          aria-label="Volume"
          onChange={(e) => controls.setVolume(Number(e.target.value))}
        />

        <div className="aplayer__rates">
          {RATES.map((r) => (
            <button
              key={r}
              type="button"
              className={r === state.rate ? 'aplayer__rate is-active' : 'aplayer__rate'}
              onClick={() => controls.setRate(r)}
            >
              {r}×
            </button>
          ))}
        </div>
      </div>

      {state.error && (
        <div className="aplayer__error">
          {state.error} Use “Open with” to play it in a media app.
        </div>
      )}
    </div>
  );
}
