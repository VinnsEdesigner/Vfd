/**
 * VideoPlayer tests.
 *
 * The orientation control is the one that changes how the video sits in the
 * window, so it is worth pinning: two modes — upright, and turned sideways so a
 * phone held upright shows the video the way a desktop screen does. The picture
 * is always fitted, never cropped.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { VideoPlayer } from './VideoPlayer';

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
