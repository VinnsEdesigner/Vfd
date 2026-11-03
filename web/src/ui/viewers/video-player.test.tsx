/**
 * VideoPlayer tests.
 *
 * The orientation control is the one that changes how the video sits in the
 * window, so it is worth pinning: two modes — upright, and turned sideways so a
 * phone held upright shows the video the way a desktop screen does. The picture
 * is always fitted, never cropped.
 */

import { render, screen, fireEvent, act } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { VideoPlayer } from './VideoPlayer';

/** The control bar, whether or not it is currently showing. */
function bar(container: HTMLElement): HTMLElement {
  return container.querySelector('.vplayer__bar') as HTMLElement;
}

describe('VideoPlayer controls visibility', () => {
  afterEach(() => {
    vi.useRealTimers();
  });

  it('shows the controls to begin with', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    expect(bar(container).className).not.toContain('is-hidden');
  });

  it('keeps the controls up while paused — that is when they are wanted', () => {
    vi.useFakeTimers();
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    act(() => {
      vi.advanceTimersByTime(10_000);
    });
    expect(bar(container).className).not.toContain('is-hidden');
  });

  it('hides the controls while playing, and a tap brings them back', () => {
    vi.useFakeTimers();
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    const video = container.querySelector('video') as HTMLVideoElement;
    // jsdom does not play, so report the state a real element would.
    Object.defineProperty(video, 'paused', { configurable: true, value: false });

    // Start playing, then let the hide timer run: the state update has to flush
    // before the timer is advanced, or the timeout is scheduled too late.
    act(() => {
      video.dispatchEvent(new Event('play'));
    });
    act(() => {
      vi.advanceTimersByTime(3_500);
    });
    expect(bar(container).className).toContain('is-hidden');

    // A tap on the picture reveals them; it must not also toggle playback, or
    // reaching for the pause button would pause on the way.
    act(() => {
      fireEvent.click(video);
    });
    expect(bar(container).className).not.toContain('is-hidden');
  });

  it('hides them from assistive tech while invisible', () => {
    vi.useFakeTimers();
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    const video = container.querySelector('video') as HTMLVideoElement;
    Object.defineProperty(video, 'paused', { configurable: true, value: false });
    act(() => {
      video.dispatchEvent(new Event('play'));
    });
    act(() => {
      vi.advanceTimersByTime(3_500);
    });
    expect(bar(container).getAttribute('aria-hidden')).toBe('true');
  });
});

describe('VideoPlayer orientation', () => {
  it('starts upright and offers a sideways control', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    const player = container.querySelector('.vplayer') as HTMLElement;
    expect(player.className).not.toContain('is-landscape');
    expect(screen.getByRole('button', { name: /turn sideways/i })).toBeTruthy();
  });

  it('turns sideways and back', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    const player = container.querySelector('.vplayer') as HTMLElement;

    fireEvent.click(screen.getByRole('button', { name: /turn sideways/i }));
    expect(player.className).toContain('is-landscape');
    expect(screen.getByRole('button', { name: /upright/i })).toBeTruthy();

    fireEvent.click(screen.getByRole('button', { name: /upright/i }));
    expect(player.className).not.toContain('is-landscape');
  });

  it('never crops the picture — the video stays fitted', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    fireEvent.click(screen.getByRole('button', { name: /turn sideways/i }));
    const video = container.querySelector('video') as HTMLVideoElement;
    // Sideways changes the box the video is drawn in, not how much is shown.
    expect(video.className).toBe('vplayer__el');
  });

  it('reflects the mode for assistive tech', () => {
    render(<VideoPlayer src="http://x/a.mp4" />);
    const toggle = screen.getByRole('button', { name: /turn sideways/i });
    expect(toggle.getAttribute('aria-pressed')).toBe('false');
    fireEvent.click(toggle);
    expect(
      screen.getByRole('button', { name: /upright/i }).getAttribute('aria-pressed'),
    ).toBe('true');
  });
});
