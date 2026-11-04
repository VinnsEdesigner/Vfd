/**
 * Live-session helpers that are pure and worth pinning.
 *
 * `mobileViewport` is the fix for the awkward mobile behaviour: the page used to
 * be re-fitted to a stage that changes size as the URL bar and keyboard move, so
 * the layout shifted under the user's finger. It is now given a fixed, phone-
 * shaped viewport, and these check the values it will and will not accept.
 */

import { describe, expect, it, vi, afterEach } from 'vitest';
import { isTouchDevice, mobileViewport } from './use-live';

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('mobileViewport', () => {
  it('uses the device’s own CSS viewport when it is sane', () => {
    vi.stubGlobal('window', { innerWidth: 412, innerHeight: 915 });
    expect(mobileViewport()).toEqual({ width: 412, height: 915 });
  });

  it('falls back to a phone shape when the viewport is nonsense', () => {
    // A 0 during startup, or a width that cannot be a phone.
    vi.stubGlobal('window', { innerWidth: 0, innerHeight: 0 });
    expect(mobileViewport()).toEqual({ width: 390, height: 844 });
    vi.stubGlobal('window', { innerWidth: 100, innerHeight: 100 });
    expect(mobileViewport()).toEqual({ width: 390, height: 844 });
  });

  it('clamps a desktop-sized viewport reported on a coarse pointer', () => {
    // A tablet reporting a desktop-class width should still get a tablet-ish
    // viewport, not a 1440-wide "phone".
    vi.stubGlobal('window', { innerWidth: 1440, innerHeight: 2000 });
    expect(mobileViewport()).toEqual({ width: 768, height: 1024 });
  });

  it('falls back when there is no window at all', () => {
    vi.stubGlobal('window', undefined);
    expect(mobileViewport()).toEqual({ width: 390, height: 844 });
  });
});

describe('isTouchDevice', () => {
  it('is true for a coarse pointer', () => {
    vi.stubGlobal('window', {
      matchMedia: (q: string) => ({ matches: q === '(pointer: coarse)' }),
    });
    expect(isTouchDevice()).toBe(true);
  });

  it('is true when the browser exposes touch events', () => {
    vi.stubGlobal('window', {
      matchMedia: () => ({ matches: false }),
      ontouchstart: null,
    });
    expect(isTouchDevice()).toBe(true);
  });

  it('is false for a plain desktop', () => {
    vi.stubGlobal('window', { matchMedia: () => ({ matches: false }) });
    expect(isTouchDevice()).toBe(false);
  });
});
