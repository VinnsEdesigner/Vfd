/**
 * VideoPlayer tests.
 *
 * Rotation is the one control that changes the element's geometry rather than
 * its playback state, so it is worth pinning: a quarter turn must transpose the
 * video to the stage's box, or a landscape clip gets clipped to a portrait
 * height. The other controls are exercised through useMediaElement.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { VideoPlayer } from './VideoPlayer';

describe('VideoPlayer rotation', () => {
  it('offers a rotate control', () => {
    render(<VideoPlayer src="http://x/a.mp4" />);
    expect(screen.getByRole('button', { name: /rotate/i })).toBeTruthy();
  });

  it('turns the video a quarter at a time and cycles back to upright', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    const video = container.querySelector('video') as HTMLVideoElement;
    const rotate = screen.getByRole('button', { name: /rotate/i });

    expect(video.style.transform).toBe('rotate(0deg)');
    fireEvent.click(rotate);
    expect(video.style.transform).toBe('rotate(90deg)');
    fireEvent.click(rotate);
    expect(video.style.transform).toBe('rotate(180deg)');
    fireEvent.click(rotate);
    expect(video.style.transform).toBe('rotate(270deg)');
    fireEvent.click(rotate);
    expect(video.style.transform).toBe('rotate(0deg)');
  });

  it('marks the stage as rotated so the layout can compensate', () => {
    const { container } = render(<VideoPlayer src="http://x/a.mp4" />);
    const stage = container.querySelector('.vplayer') as HTMLElement;
    expect(stage.className).not.toContain('is-rotated');
    fireEvent.click(screen.getByRole('button', { name: /rotate/i }));
    expect(stage.className).toContain('is-rotated');
  });
});
