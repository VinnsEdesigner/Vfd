/**
 * VideoPlayer tests.
 *
 * The fit/fill control is the one that changes how the frame meets the screen,
 * so it is worth pinning: a player's aspect control has exactly two modes, and
 * the video element has to actually carry the mode.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { VideoPlayer } from './VideoPlayer';

describe('VideoPlayer fit/fill', () => {
  it('offers a fit control, starting on fit', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    const video = container.querySelector('video') as HTMLVideoElement;
    // Fit is the default: the whole picture, letterboxed.
    expect(video.className).toBe('vplayer__el');
    expect(screen.getByRole('button', { name: /fill the screen/i })).toBeTruthy();
  });

  it('switches to fill and back', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    const video = container.querySelector('video') as HTMLVideoElement;

    fireEvent.click(screen.getByRole('button', { name: /fill the screen/i }));
    expect(video.className).toContain('is-fill');
    expect(screen.getByRole('button', { name: /fit to screen/i })).toBeTruthy();

    fireEvent.click(screen.getByRole('button', { name: /fit to screen/i }));
    expect(video.className).not.toContain('is-fill');
  });

  it('reflects the mode for assistive tech', () => {
    render(<VideoPlayer src="http://x/a.mp4" />);
    const toggle = screen.getByRole('button', { name: /fill the screen/i });
    expect(toggle.getAttribute('aria-pressed')).toBe('false');
    fireEvent.click(toggle);
    expect(
      screen.getByRole('button', { name: /fit to screen/i }).getAttribute('aria-pressed'),
    ).toBe('true');
  });
});
