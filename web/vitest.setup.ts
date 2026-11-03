import '@testing-library/react';

/**
 * jsdom implements neither PointerEvent nor the pointer-capture methods, but the
 * viewers are built on pointer events (a mouse and a finger are the same thing to
 * them). Without this, any gesture test throws `PointerEvent is not defined` and
 * the gesture code goes untested.
 *
 * The polyfill extends MouseEvent, which jsdom does provide, and adds the pointer
 * fields the components read. Capture methods are no-ops: they only affect where
 * later events are delivered, which a test drives by hand anyway.
 */
if (typeof globalThis.PointerEvent !== 'function') {
  class PointerEventPolyfill extends MouseEvent {
    pointerId: number;
    pointerType: string;
    isPrimary: boolean;
    width: number;
    height: number;
    pressure: number;

    constructor(type: string, params: PointerEventInit = {}) {
      super(type, params);
      this.pointerId = params.pointerId ?? 1;
      this.pointerType = params.pointerType ?? 'mouse';
      this.isPrimary = params.isPrimary ?? true;
      this.width = params.width ?? 1;
      this.height = params.height ?? 1;
      this.pressure = params.pressure ?? 0.5;
    }
  }
  Object.defineProperty(globalThis, 'PointerEvent', {
    configurable: true,
    writable: true,
    value: PointerEventPolyfill,
  });
}

// jsdom's Element lacks these; the viewers call them inside try/catch, but a
// missing method still throws a TypeError rather than a caught DOMException.
if (typeof Element !== 'undefined') {
  const proto = Element.prototype as unknown as Record<string, unknown>;
  for (const name of ['setPointerCapture', 'releasePointerCapture', 'hasPointerCapture']) {
    if (typeof proto[name] !== 'function') {
      Object.defineProperty(Element.prototype, name, {
        configurable: true,
        writable: true,
        value: () => undefined,
      });
    }
  }
}

/**
 * jsdom does not implement the object-URL API, and the storage layer uses it to
 * hand media to a player. Providing a real-enough stub here keeps the tests
 * exercising the actual code path instead of skipping it.
 */
if (typeof URL.createObjectURL !== 'function') {
  let counter = 0;
  const registry = new Map<string, Blob>();
  Object.defineProperty(URL, 'createObjectURL', {
    configurable: true,
    writable: true,
    value: (blob: Blob): string => {
      const url = `blob:vyzorix/${counter++}`;
      registry.set(url, blob);
      return url;
    },
  });
  Object.defineProperty(URL, 'revokeObjectURL', {
    configurable: true,
    writable: true,
    value: (url: string): void => {
      registry.delete(url);
    },
  });
}
