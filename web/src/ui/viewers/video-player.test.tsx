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
import { VideoPlayer, detectPipSupport } from './VideoPlayer';

/** The control bar, whether or not it is currently showing. */
function bar(container: HTMLElement): HTMLElement {
  return container.querySelector('.vplayer__bar') as HTMLElement;
}

function videoEl(container: HTMLElement): HTMLVideoElement {
  return container.querySelector('video') as HTMLVideoElement;
}

/** The physical filter factor on the picture: 1 is the source, untouched. */
function filterFactor(video: HTMLVideoElement): number {
  const m = /brightness\(([\d.]+)\)/.exec(video.style.filter);
  return m ? Number(m[1]) : 1;
}

/** The level the UI reports, 0..1, as read off the indicator. */
function hudLevel(container: HTMLElement): number | null {
  const el = container.querySelector('.vplayer__hud-value');
  return el ? Number(el.textContent!.replace('%', '')) / 100 : null;
}

/**
 * Make the picture usable in jsdom: a real box (jsdom has no layout, so the
 * frame would report zero and the gesture's side and span would both be
 * degenerate) and a `play()` that returns a promise, which jsdom's does not.
 */
function withFrameBox(container: HTMLElement, width = 400, height = 300) {
  const frame = container.querySelector('.vplayer__frame') as HTMLElement;
  frame.getBoundingClientRect = () =>
    ({ left: 0, top: 0, width, height, right: width, bottom: height, x: 0, y: 0 }) as DOMRect;
  const video = videoEl(container);
  video.play = vi.fn().mockResolvedValue(undefined);
  video.pause = vi.fn();
}

function pointer(type: string, x: number, y: number) {
  return new PointerEvent(type, { pointerId: 1, pointerType: 'touch', clientX: x, clientY: y, bubbles: true });
}

/** A press and release with no movement: a tap. */
function tap(el: HTMLElement, x = 200, y = 150) {
  el.dispatchEvent(pointer('pointerdown', x, y));
  el.dispatchEvent(pointer('pointerup', x, y));
}

/**
 * A vertical swipe. `from` and `to` are y positions; x picks the half of the
 * picture, which decides whether it is brightness or volume.
 */
function swipe(el: HTMLElement, x: number, fromY: number, toY: number) {
  el.dispatchEvent(pointer('pointerdown', x, fromY));
  const steps = 5;
  for (let i = 1; i <= steps; i++) {
    el.dispatchEvent(pointer('pointermove', x, fromY + ((toY - fromY) * i) / steps));
  }
  el.dispatchEvent(pointer('pointerup', x, toY));
}

describe('detectPipSupport', () => {
  afterEach(() => {
    // `pictureInPictureEnabled` is read-only, so restore it explicitly.
    Object.defineProperty(document, 'pictureInPictureEnabled', {
      configurable: true,
      value: undefined,
    });
  });

  const video = () => document.createElement('video');

  it('reports nothing for a missing element', () => {
    expect(detectPipSupport(null)).toBe('none');
  });

  it('reports the standard API when the document enables it', () => {
    Object.defineProperty(document, 'pictureInPictureEnabled', {
      configurable: true,
      value: true,
    });
    const el = video();
    (el as HTMLVideoElement & { requestPictureInPicture: () => Promise<unknown> }).requestPictureInPicture =
      () => Promise.resolve({} as PictureInPictureWindow);
    expect(detectPipSupport(el)).toBe('standard');
  });

  it('does not claim the standard API when the document disables it', () => {
    // This is the mobile-Chrome case: the method may exist but the feature is off.
    Object.defineProperty(document, 'pictureInPictureEnabled', {
      configurable: true,
      value: false,
    });
    const el = video();
    (el as HTMLVideoElement & { requestPictureInPicture: () => Promise<unknown> }).requestPictureInPicture =
      () => Promise.resolve({} as PictureInPictureWindow);
    expect(detectPipSupport(el)).toBe('none');
  });

  it('reports the webkit API for Safari', () => {
    Object.defineProperty(document, 'pictureInPictureEnabled', {
      configurable: true,
      value: false,
    });
    const el = video() as HTMLVideoElement & {
      webkitSupportsPresentationMode: (m: string) => boolean;
    };
    el.webkitSupportsPresentationMode = () => true;
    expect(detectPipSupport(el)).toBe('webkit');
  });

  it('reports nothing when webkit refuses the mode', () => {
    Object.defineProperty(document, 'pictureInPictureEnabled', {
      configurable: true,
      value: false,
    });
    const el = video() as HTMLVideoElement & {
      webkitSupportsPresentationMode: (m: string) => boolean;
    };
    el.webkitSupportsPresentationMode = () => false;
    expect(detectPipSupport(el)).toBe('none');
  });
});

describe('VideoPlayer picture-in-picture control', () => {
  afterEach(() => {
    Object.defineProperty(document, 'pictureInPictureEnabled', {
      configurable: true,
      value: undefined,
    });
  });

  it('is not offered where the browser cannot do it', () => {
    Object.defineProperty(document, 'pictureInPictureEnabled', {
      configurable: true,
      value: false,
    });
    render(<VideoPlayer src="http://x/a.mp4" />);
    expect(screen.queryByRole('button', { name: /picture in picture/i })).toBeNull();
  });
});

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
    // reaching for the pause button would pause on the way. The tap is a press
    // and release with no movement, which is what a finger actually does.
    act(() => {
      tap(video);
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

describe('VideoPlayer brightness and volume swipe', () => {
  afterEach(() => {
    vi.useRealTimers();
  });

  it('opens at the source, with the level at the midpoint', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    // 50% is the untouched source, which is what leaves headroom to lift.
    expect(filterFactor(videoEl(container))).toBeCloseTo(1, 5);
  });

  it('dims the picture on a left-half swipe down, and shows the level', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    // Left half, dragged down: 160px of a 300px frame over a 1.4x range.
    act(() => {
      swipe(video, 60, 100, 260);
    });
    expect(filterFactor(video)).toBeLessThan(1);
    expect(hudLevel(container)).toBeLessThan(0.5);
    expect(screen.getByLabelText(/^Brightness \d+%$/)).toBeTruthy();
  });

  it('lifts the picture above the source on a swipe up, so a dim screen glows', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    act(() => {
      swipe(video, 60, 260, 60);
    });
    // Past the source: this is the "boost" that a screen-brightness API would
    // have given, done to the picture instead.
    expect(filterFactor(video)).toBeGreaterThan(1);
  });

  it('never reports a level above 100%', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    // Far more than a full-height swipe, several times over.
    for (let i = 0; i < 4; i++) {
      act(() => {
        swipe(video, 60, 290, 10);
      });
    }
    expect(hudLevel(container)).toBeLessThanOrEqual(1);
    expect(screen.getByLabelText('Brightness 100%')).toBeTruthy();
  });

  it('never reports a level below 0%', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    for (let i = 0; i < 4; i++) {
      act(() => {
        swipe(video, 60, 10, 290);
      });
    }
    expect(hudLevel(container)).toBeGreaterThanOrEqual(0);
  });

  it('sets volume on a right-half swipe, and leaves brightness alone', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    // Down first, so "up" has somewhere to go from a known volume.
    act(() => {
      swipe(video, 340, 150, 250);
    });
    const quiet = video.volume;
    act(() => {
      swipe(video, 340, 250, 60);
    });
    expect(video.volume).toBeGreaterThan(quiet);
    // The right half must never touch the picture's brightness.
    expect(filterFactor(video)).toBeCloseTo(1, 5); // unchanged: still the source
    expect(screen.getByLabelText(/^Volume \d+%$/)).toBeTruthy();
  });

  it('keeps the volume within 0..1 however far the swipe goes', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    act(() => {
      swipe(video, 340, 290, 10);
    });
    expect(video.volume).toBeLessThanOrEqual(1);
    act(() => {
      swipe(video, 340, 10, 290);
    });
    expect(video.volume).toBeGreaterThanOrEqual(0);
  });

  it('treats a press with no movement as a tap, not a level change', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    act(() => {
      tap(video, 60, 150);
    });
    expect(filterFactor(video)).toBeCloseTo(1, 5); // unchanged: still the source
    expect(container.querySelector('.vplayer__hud')).toBeNull();
  });

  it('ignores a movement too small to be a swipe', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    act(() => {
      video.dispatchEvent(pointer('pointerdown', 60, 150));
      video.dispatchEvent(pointer('pointermove', 60, 145));
      video.dispatchEvent(pointer('pointerup', 60, 145));
    });
    expect(filterFactor(video)).toBeCloseTo(1, 5); // unchanged: still the source
  });

  it('drives the fill from the level, so one markup serves both layouts', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    act(() => {
      swipe(videoEl(container), 60, 250, 60);
    });
    // The track is vertical in portrait and horizontal in landscape, so the
    // level is a variable the stylesheet maps onto the right dimension; the
    // markup itself does not change with orientation.
    const fill = container.querySelector('.vplayer__hud-fill') as HTMLElement;
    expect(fill.style.getPropertyValue('--level')).toBeTruthy();
    expect(Number(fill.style.getPropertyValue('--level'))).toBeGreaterThan(0.5);
  });

  it('hides the indicator again after a moment', () => {
    vi.useFakeTimers();
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    act(() => {
      swipe(video, 60, 250, 120);
    });
    expect(container.querySelector('.vplayer__hud')).not.toBeNull();
    act(() => {
      vi.advanceTimersByTime(1_500);
    });
    expect(container.querySelector('.vplayer__hud')).toBeNull();
  });

  it('puts the brightness indicator on the left and volume on the right', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    withFrameBox(container);
    const video = videoEl(container);
    act(() => {
      swipe(video, 60, 250, 130);
    });
    expect(container.querySelector('.vplayer__hud--brightness')).toBeTruthy();
    act(() => {
      swipe(video, 340, 150, 250);
    });
    expect(container.querySelector('.vplayer__hud--volume')).toBeTruthy();
  });
});
