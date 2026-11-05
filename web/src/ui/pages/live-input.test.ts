/**
 * Live-input mapping tests.
 *
 * Pure functions: the arithmetic of scaling a click onto the frame's pixel grid,
 * the modifier bitfield, and the key mapping. These are the parts that are easy
 * to get subtly wrong — a modifier from the wrong source, a click off by the
 * canvas scale — and impossible to notice without driving a real page.
 */

import { describe, expect, it } from 'vitest';
import {
  keyMessage,
  modifiersFromEvent,
  mouseMessage,
  navigateMessage,
  resizeMessage,
  textMessage,
  displayedFrameRect,
  toFrameCoords,
  touchMessage,
  wheelMessage,
} from './live-input';

describe('modifiersFromEvent', () => {
  it('uses CDP bit values, not DOM ones', () => {
    // Alt 1, Ctrl 2, Meta 4, Shift 8. The DOM's own bitfield is different, so
    // reading the wrong one would send the wrong modifier.
    expect(modifiersFromEvent({ altKey: true })).toBe(1);
    expect(modifiersFromEvent({ ctrlKey: true })).toBe(2);
    expect(modifiersFromEvent({ metaKey: true })).toBe(4);
    expect(modifiersFromEvent({ shiftKey: true })).toBe(8);
    expect(modifiersFromEvent({ ctrlKey: true, shiftKey: true })).toBe(10);
    expect(modifiersFromEvent({})).toBe(0);
  });
});

describe('toFrameCoords', () => {
  const rect = { left: 100, top: 50, width: 800, height: 600 };

  it('is the identity when the canvas is shown at the frame size', () => {
    expect(toFrameCoords(100, 50, rect, 800, 600)).toEqual({ x: 0, y: 0 });
    expect(toFrameCoords(500, 350, rect, 800, 600)).toEqual({ x: 400, y: 300 });
  });

  it('scales when the canvas is shown smaller than the frame', () => {
    // A 400-wide display of an 800-wide frame: the centre is still the centre,
    // and a point a quarter in maps a quarter of the frame's width.
    const half = { left: 0, top: 0, width: 400, height: 300 };
    expect(toFrameCoords(200, 150, half, 800, 600)).toEqual({ x: 400, y: 300 });
    expect(toFrameCoords(100, 75, half, 800, 600)).toEqual({ x: 200, y: 150 });
  });

  it('clamps to the frame, so a stray event outside it does not go negative', () => {
    expect(toFrameCoords(-50, -50, rect, 800, 600)).toEqual({ x: 0, y: 0 });
    expect(toFrameCoords(9999, 9999, rect, 800, 600)).toEqual({ x: 800, y: 600 });
  });

  it('is safe on a zero-sized rect', () => {
    expect(toFrameCoords(10, 10, { left: 0, top: 0, width: 0, height: 0 }, 800, 600)).toEqual({
      x: 0,
      y: 0,
    });
  });

  it('accounts for letterboxing when the box is a different shape from the frame', () => {
    // A tall box (9:19.5) holding a wider frame (9:16): the frame is fitted by
    // width and the bars are top and bottom. Mapping against the box instead
    // would put every tap too far down — the bug this guards.
    const tall = { left: 0, top: 0, width: 400, height: 800 };
    // The frame is 400 wide and 400*16/9 = 711.1 tall, so the centre of the frame
    // is at y = 355.6, not y = 400.
    const centre = toFrameCoords(200, 355.555, tall, 900, 1600);
    expect(centre.x).toBeCloseTo(450, 1);
    expect(centre.y).toBeCloseTo(800, 1);

    // A point on the bottom edge of the *frame* is above the bottom of the box.
    const bottom = toFrameCoords(200, 711.111, tall, 900, 1600);
    expect(bottom.y).toBeCloseTo(1600, 1);
  });

  it('accounts for letterboxing when the box is wider than the frame', () => {
    // A wide box holding a tall frame: bars on the left and right. The frame is
    // pinned to the top-left (the CSS `object-position`), so the bars are all on
    // the right and the bottom.
    const wide = { left: 0, top: 0, width: 1000, height: 500 };
    // The frame is 500 tall and 500*390/844 = 231.0 wide, at the left edge.
    const centre = toFrameCoords(115.5, 250, wide, 390, 844);
    expect(centre.x).toBeCloseTo(195, 0);
    expect(centre.y).toBeCloseTo(422, 0);

    // A point inside the right-hand bar clamps to the frame's right edge rather
    // than reporting a coordinate past the page.
    const inBar = toFrameCoords(800, 250, wide, 390, 844);
    expect(inBar.x).toBe(390);
  });

  it('is the identity when the box matches the frame shape', () => {
    const box = { left: 10, top: 20, width: 400, height: 800 };
    const p = toFrameCoords(210, 420, box, 400, 800);
    expect(p.x).toBeCloseTo(200, 1);
    expect(p.y).toBeCloseTo(400, 1);
  });
});

describe('displayedFrameRect', () => {
  it('fits the frame inside the box at the frame shape', () => {
    const r = displayedFrameRect({ left: 0, top: 0, width: 400, height: 800 }, 900, 1600);
    expect(r.width).toBeCloseTo(400, 1);
    expect(r.height).toBeCloseTo(711.1, 1);
    expect(r.left).toBe(0);
    expect(r.top).toBe(0);
  });

  it('centres when asked', () => {
    const r = displayedFrameRect({ left: 0, top: 0, width: 1000, height: 500 }, 390, 844, 'center');
    expect(r.left).toBeCloseTo(384.48, 1);
    expect(r.top).toBe(0);
  });

  it('passes the box through when the frame size is unknown', () => {
    const box = { left: 1, top: 2, width: 3, height: 4 };
    expect(displayedFrameRect(box, 0, 0)).toEqual(box);
  });
});

describe('keyMessage', () => {
  const base = { key: 'a', code: 'KeyA', keyCode: 65 };

  it('sets text for a printable key on keyDown', () => {
    const m = keyMessage('keyDown', base);
    expect(m.text).toBe('a');
    expect(m.vk).toBe(65);
    expect(m.event).toBe('keyDown');
  });

  it('does not set text for a shortcut, or the page would type a "c" on Ctrl+C', () => {
    const m = keyMessage('keyDown', { ...base, key: 'c', ctrlKey: true });
    expect(m.text).toBeUndefined();
    expect(m.modifiers).toBe(2);
  });

  it('does not set text for a non-printable key', () => {
    expect(keyMessage('keyDown', { key: 'ArrowLeft', code: 'ArrowLeft', keyCode: 37 }).text).toBeUndefined();
  });

  it('does not set text on keyUp', () => {
    expect(keyMessage('keyUp', base).text).toBeUndefined();
  });
});

describe('message builders', () => {
  it('build a mouse message in Vyzor’s shape', () => {
    expect(mouseMessage('mousePressed', 10, 20, 'left', 1, 1, 0)).toEqual({
      type: 'mouse',
      event: 'mousePressed',
      x: 10,
      y: 20,
      button: 'left',
      click_count: 1,
      buttons: 1,
      modifiers: 0,
    });
  });

  it('build a wheel message', () => {
    expect(wheelMessage(1, 2, 0, 120, 0)).toEqual({
      type: 'wheel',
      x: 1,
      y: 2,
      dx: 0,
      dy: 120,
      modifiers: 0,
    });
  });

  it('round a resize to whole pixels', () => {
    expect(resizeMessage(800.6, 600.2)).toEqual({ type: 'resize', width: 801, height: 600 });
  });

  it('build text and navigate messages', () => {
    expect(textMessage('hi')).toEqual({ type: 'text', text: 'hi' });
    expect(navigateMessage('https://x/')).toEqual({ type: 'navigate', url: 'https://x/' });
  });
});

describe('touchMessage', () => {
  it('flattens points into Vyzor’s x,y list', () => {
    expect(touchMessage('touchStart', [{ x: 10, y: 20 }], 0)).toEqual({
      type: 'touch',
      event: 'touchStart',
      points: [10, 20],
      modifiers: 0,
    });
  });

  it('carries two fingers as four numbers', () => {
    // A pinch needs both points in one message; Vyzor reads them pairwise.
    const m = touchMessage('touchMove', [{ x: 1, y: 2 }, { x: 3, y: 4 }], 0);
    expect(m.points).toEqual([1, 2, 3, 4]);
  });

  it('names the event Vyzor expects', () => {
    for (const e of ['touchStart', 'touchMove', 'touchEnd', 'touchCancel'] as const) {
      expect(touchMessage(e, [{ x: 0, y: 0 }], 0).event).toBe(e);
    }
  });
});
