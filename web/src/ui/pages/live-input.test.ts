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
  it('build a mouse message in the engine’s shape', () => {
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
  it('flattens points into the engine’s x,y list', () => {
    expect(touchMessage('touchStart', [{ x: 10, y: 20 }], 0)).toEqual({
      type: 'touch',
      event: 'touchStart',
      points: [10, 20],
      modifiers: 0,
    });
  });

  it('carries two fingers as four numbers', () => {
    // A pinch needs both points in one message; the engine reads them pairwise.
    const m = touchMessage('touchMove', [{ x: 1, y: 2 }, { x: 3, y: 4 }], 0);
    expect(m.points).toEqual([1, 2, 3, 4]);
  });

  it('names the event the engine expects', () => {
    for (const e of ['touchStart', 'touchMove', 'touchEnd', 'touchCancel'] as const) {
      expect(touchMessage(e, [{ x: 0, y: 0 }], 0).event).toBe(e);
    }
  });
});
