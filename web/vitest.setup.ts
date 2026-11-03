import '@testing-library/react';

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
