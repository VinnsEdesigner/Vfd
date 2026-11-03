/**
 * ImageViewer gesture tests.
 *
 * Pinch is the reason this file exists. The viewer had zoom buttons and drag but
 * no pinch at all — the CSS even had an `.is-pinching` rule that nothing ever
 * set. The maths is `scale * (fingerDistance / startDistance)`, so a 2x spread
 * must give exactly 2x the scale; asserting "it changed" would pass for a wrong
 * ratio, which is how it went unnoticed.
 */

import { render, screen, fireEvent, act } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { ImageViewer } from './ImageViewer';

function stage(container: HTMLElement): HTMLElement {
  return container.querySelector('.imgstage') as HTMLElement;
}

function img(container: HTMLElement): HTMLElement {
  return container.querySelector('.imgstage__img') as HTMLElement;
}

function scaleOf(container: HTMLElement): number {
  const t = img(container).style.transform;
  const m = /scale\(([\d.]+)\)/.exec(t);
  return m ? Number(m[1]) : NaN;
}

/** A pointer event at a point, with an id so two can coexist. */
function pt(type: string, id: number, x: number, y: number) {
  return new PointerEvent(type, {
    pointerId: id,
    pointerType: 'touch',
    clientX: x,
    clientY: y,
    bubbles: true,
  });
}

/**
 * Dispatch one gesture event and let React flush.
 *
 * React batches state updates inside a single `act()`, so dispatching a whole
 * gesture in one block and then reading the transform reads it *before* the
 * re-render — the assertion sees the previous frame and a working pan looks
 * broken. One event per `act()` is what a real gesture does.
 */
function step(el: HTMLElement, ev: PointerEvent) {
  act(() => {
    el.dispatchEvent(ev);
  });
}

/**
 * jsdom has no layout, so the stage reports a zero-sized rect. Pinch maths only
 * needs a centre, and the anchor offset is a no-op when the rect is degenerate —
 * the scale itself is what these tests pin.
 */
function pinchBy(container: HTMLElement, factor: number) {
  const el = stage(container);
  const start = 40;
  const cx = 200;
  const cy = 200;
  step(el, pt('pointerdown', 1, cx - start, cy));
  step(el, pt('pointerdown', 2, cx + start, cy));
  const steps = 6;
  for (let i = 1; i <= steps; i++) {
    const d = start + ((start * factor - start) * i) / steps;
    step(el, pt('pointermove', 1, cx - d, cy));
    step(el, pt('pointermove', 2, cx + d, cy));
  }
  step(el, pt('pointerup', 1, cx - start * factor, cy));
  step(el, pt('pointerup', 2, cx + start * factor, cy));
}

describe('ImageViewer pinch', () => {
  it('scales by exactly the finger-spread ratio', () => {
    const { container } = render(<ImageViewer src="http://x/a.png" alt="a" />);
    expect(scaleOf(container)).toBe(1);
    pinchBy(container, 2);
    expect(scaleOf(container)).toBeCloseTo(2, 2);
  });

  it('compounds successive pinches, and reverses on a pinch in', () => {
    const { container } = render(<ImageViewer src="http://x/a.png" alt="a" />);
    pinchBy(container, 2);
    pinchBy(container, 2);
    expect(scaleOf(container)).toBeCloseTo(4, 2);
    pinchBy(container, 0.5);
    expect(scaleOf(container)).toBeCloseTo(2, 2);
  });

  it('marks the stage as pinching so the CSS cursor follows', () => {
    const { container } = render(<ImageViewer src="http://x/a.png" alt="a" />);
    const el = stage(container);
    act(() => {
      el.dispatchEvent(pt('pointerdown', 1, 160, 200));
      el.dispatchEvent(pt('pointerdown', 2, 240, 200));
    });
    expect(el.className).toContain('is-pinching');
    act(() => {
      el.dispatchEvent(pt('pointerup', 1, 160, 200));
      el.dispatchEvent(pt('pointerup', 2, 240, 200));
    });
    expect(el.className).not.toContain('is-pinching');
  });

  it('does not exceed the maximum scale', () => {
    const { container } = render(<ImageViewer src="http://x/a.png" alt="a" />);
    for (let i = 0; i < 8; i++) pinchBy(container, 2);
    expect(scaleOf(container)).toBeLessThanOrEqual(8);
  });
});

describe('ImageViewer drag', () => {
  it('pans with one pointer once zoomed in', () => {
    const { container } = render(<ImageViewer src="http://x/a.png" alt="a" />);
    fireEvent.click(screen.getByRole('button', { name: /zoom in/i }));
    const el = stage(container);
    step(el, pt('pointerdown', 1, 200, 200));
    step(el, pt('pointermove', 1, 260, 230));
    step(el, pt('pointerup', 1, 260, 230));
    // The translate moved; the scale did not.
    expect(img(container).style.transform).toContain('translate(60px, 30px)');
  });

  it('does not pan at 100%, because nothing would move', () => {
    const { container } = render(<ImageViewer src="http://x/a.png" alt="a" />);
    const el = stage(container);
    step(el, pt('pointerdown', 1, 200, 200));
    step(el, pt('pointermove', 1, 280, 240));
    step(el, pt('pointerup', 1, 280, 240));
    expect(img(container).style.transform).toContain('translate(0px, 0px)');
  });

  it('keeps panning with the finger left after a pinch ends', () => {
    const { container } = render(<ImageViewer src="http://x/a.png" alt="a" />);
    const el = stage(container);
    // Pinch to zoom, then lift one finger.
    step(el, pt('pointerdown', 1, 160, 200));
    step(el, pt('pointerdown', 2, 240, 200));
    step(el, pt('pointermove', 1, 120, 200));
    step(el, pt('pointermove', 2, 280, 200));
    step(el, pt('pointerup', 2, 280, 200));
    const before = img(container).style.transform;
    // The finger that is still down pans from where it is, not from the pinch.
    step(el, pt('pointermove', 1, 160, 240));
    step(el, pt('pointerup', 1, 160, 240));
    expect(img(container).style.transform).not.toBe(before);
  });
});

describe('ImageViewer zoom controls', () => {
  it('zooms with the buttons and reports the percentage', () => {
    render(<ImageViewer src="http://x/a.png" alt="a" />);
    expect(screen.getByText('100%')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: /zoom in/i }));
    expect(screen.getByText('120%')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: /zoom out/i }));
    expect(screen.getByText('100%')).toBeTruthy();
  });

  it('resets rotation and scale together', () => {
    const { container } = render(<ImageViewer src="http://x/a.png" alt="a" />);
    fireEvent.click(screen.getByRole('button', { name: /zoom in/i }));
    fireEvent.click(screen.getByRole('button', { name: /rotate/i }));
    fireEvent.click(screen.getByRole('button', { name: /reset view/i }));
    expect(scaleOf(container)).toBe(1);
    expect(img(container).style.transform).toContain('rotate(0deg)');
  });

  it('reports the natural size once the image loads', () => {
    const onDimensions = vi.fn();
    const { container } = render(
      <ImageViewer src="http://x/a.png" alt="a" onDimensions={onDimensions} />,
    );
    const el = img(container) as HTMLImageElement;
    Object.defineProperty(el, 'naturalWidth', { configurable: true, value: 800 });
    Object.defineProperty(el, 'naturalHeight', { configurable: true, value: 600 });
    fireEvent.load(el);
    expect(onDimensions).toHaveBeenCalledWith({ width: 800, height: 600 });
    expect(screen.getByText('800×600')).toBeTruthy();
  });
});
