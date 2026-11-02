/**
 * useMediaElement — playback state for an <audio>/<video> element.
 *
 * Both players need the same things (play state, current time, duration,
 * volume, rate, seeking, error), so the logic lives here once rather than
 * being duplicated and drifting.
 */

import { useCallback, useEffect, useRef, useState } from 'react';

export interface MediaElementState {
  playing: boolean;
  currentTime: number;
  duration: number;
  volume: number;
  muted: boolean;
  rate: number;
  buffered: number;
  error: string | null;
  ready: boolean;
}

export interface MediaElementControls {
  toggle: () => void;
  play: () => void;
  pause: () => void;
  seek: (seconds: number) => void;
  seekBy: (delta: number) => void;
  setVolume: (v: number) => void;
  toggleMute: () => void;
  setRate: (r: number) => void;
  /** Attach to the element's ref. */
  bind: (el: HTMLMediaElement | null) => void;
}

const RATES = [0.5, 0.75, 1, 1.25, 1.5, 2] as const;

export { RATES };

export function useMediaElement(): [MediaElementState, MediaElementControls] {
  const elRef = useRef<HTMLMediaElement | null>(null);
  const [state, setState] = useState<MediaElementState>({
    playing: false,
    currentTime: 0,
    duration: 0,
    volume: 1,
    muted: false,
    rate: 1,
    buffered: 0,
    error: null,
    ready: false,
  });

  const sync = useCallback(() => {
    const el = elRef.current;
    if (!el) return;
    let buffered = 0;
    if (el.buffered.length > 0) {
      // The buffered range covering the playhead is the useful one.
      for (let i = 0; i < el.buffered.length; i++) {
        if (el.buffered.start(i) <= el.currentTime && el.currentTime <= el.buffered.end(i)) {
          buffered = el.buffered.end(i);
          break;
        }
      }
    }
    setState((s) => ({
      ...s,
      playing: !el.paused && !el.ended,
      currentTime: el.currentTime,
      duration: Number.isFinite(el.duration) ? el.duration : 0,
      volume: el.volume,
      muted: el.muted,
      rate: el.playbackRate,
      buffered,
      ready: el.readyState >= 1,
    }));
  }, []);

  const bind = useCallback(
    (el: HTMLMediaElement | null) => {
      elRef.current = el;
      if (!el) return;
      sync();
    },
    [sync],
  );

  // Element events are the source of truth; a light interval only smooths the
  // progress bar while playing.
  useEffect(() => {
    const el = elRef.current;
    if (!el) return;
    const events = [
      'play', 'pause', 'ended', 'loadedmetadata', 'durationchange',
      'timeupdate', 'volumechange', 'ratechange', 'progress', 'seeked', 'error',
    ];
    events.forEach((e) => el.addEventListener(e, sync));
    const onError = () => {
      const code = el.error?.code;
      setState((s) => ({
        ...s,
        error:
          code === 4
            ? 'This file cannot be decoded by the browser.'
            : 'Playback failed.',
      }));
    };
    el.addEventListener('error', onError);
    return () => {
      events.forEach((e) => el.removeEventListener(e, sync));
      el.removeEventListener('error', onError);
    };
  }, [sync, state.ready]);

  useEffect(() => {
    if (!state.playing) return;
    const id = window.setInterval(sync, 250);
    return () => window.clearInterval(id);
  }, [state.playing, sync]);

  const controls: MediaElementControls = {
    bind,
    toggle: () => {
      const el = elRef.current;
      if (!el) return;
      if (el.paused) void el.play().catch(() => setState((s) => ({ ...s, error: 'Autoplay was blocked.' })));
      else el.pause();
    },
    play: () => {
      void elRef.current?.play().catch(() => undefined);
    },
    pause: () => elRef.current?.pause(),
    seek: (seconds) => {
      const el = elRef.current;
      if (el && Number.isFinite(seconds)) el.currentTime = Math.max(0, seconds);
    },
    seekBy: (delta) => {
      const el = elRef.current;
      if (el) el.currentTime = Math.max(0, Math.min(el.duration || 0, el.currentTime + delta));
    },
    setVolume: (v) => {
      const el = elRef.current;
      if (el) {
        el.volume = Math.min(1, Math.max(0, v));
        if (v > 0) el.muted = false;
      }
    },
    toggleMute: () => {
      const el = elRef.current;
      if (el) el.muted = !el.muted;
    },
    setRate: (r) => {
      const el = elRef.current;
      if (el) el.playbackRate = r;
    },
  };

  return [state, controls];
}
