/**
 * The connection store, and the one thing that bit us in production.
 *
 * A saved endpoint used to win over the build-time one unconditionally. That is
 * fine until a redeploy bakes in a *different* Vyzor address: the browser then
 * keeps talking to the old one, which may no longer exist, while the UI insists
 * it is configured. The store now stamps a saved connection with the build it
 * was saved for and discards anything else.
 *
 * These tests pin that, including the case that caused the outage: a value
 * saved before the stamp existed must be treated as stale, not trusted.
 */

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const KEY = 'vyzorix.media.connection';

/** A minimal localStorage, since jsdom's is per-file and we replace it. */
function fakeStorage(): Storage {
  const map = new Map<string, string>();
  return {
    get length() {
      return map.size;
    },
    clear: () => map.clear(),
    getItem: (k: string) => map.get(k) ?? null,
    key: (i: number) => [...map.keys()][i] ?? null,
    removeItem: (k: string) => void map.delete(k),
    setItem: (k: string, v: string) => void map.set(k, v),
  } as Storage;
}

/**
 * Loads the module fresh, with `VITE_API_BASE_URL` set to `baked`.
 *
 * The store reads its default at import time, so the only way to test a
 * different baked URL is to re-import with the env stubbed.
 */
async function loadStore(baked: string, seed?: string) {
  vi.resetModules();
  vi.stubGlobal('localStorage', fakeStorage());
  if (seed !== undefined) localStorage.setItem(KEY, seed);
  vi.stubEnv('VITE_API_BASE_URL', baked);
  return import('./connection-store');
}

beforeEach(() => {
  vi.unstubAllEnvs();
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.unstubAllEnvs();
});

describe('connection store', () => {
  it('uses the baked URL when nothing is saved', async () => {
    const { useConnectionStore } = await loadStore('https://vyzor.example');
    expect(useConnectionStore.getState().baseUrl).toBe('https://vyzor.example');
  });

  it('keeps a saved endpoint that belongs to this build', async () => {
    const { useConnectionStore } = await loadStore(
      'https://vyzor.example',
      JSON.stringify({ baseUrl: 'https://other.example', bakedFor: 'https://vyzor.example' }),
    );
    expect(useConnectionStore.getState().baseUrl).toBe('https://other.example');
  });

  it('discards a saved endpoint from a different build', async () => {
    // The exact production failure: a value saved against an older build would
    // otherwise win forever, even after a redeploy bakes in the right address.
    const { useConnectionStore } = await loadStore(
      'https://work-1-new.prod-runtime.all-hands.dev',
      JSON.stringify({
        baseUrl: 'https://vyzorix-file-downloader.pages.dev',
        bakedFor: 'https://work-1-old.prod-runtime.all-hands.dev',
      }),
    );
    expect(useConnectionStore.getState().baseUrl).toBe(
      'https://work-1-new.prod-runtime.all-hands.dev',
    );
  });

  it('treats a value saved before the stamp existed as stale', async () => {
    // No `bakedFor` means the provenance is unknown, so it must not be trusted.
    const { useConnectionStore } = await loadStore(
      'https://vyzor.example',
      JSON.stringify({ baseUrl: 'https://stale.example' }),
    );
    expect(useConnectionStore.getState().baseUrl).toBe('https://vyzor.example');
  });

  it('stamps what it saves, so the next load can judge it', async () => {
    const { useConnectionStore } = await loadStore('https://vyzor.example');
    useConnectionStore.getState().setBaseUrl('https://chosen.example');
    const saved = JSON.parse(localStorage.getItem(KEY) ?? '{}');
    expect(saved.baseUrl).toBe('https://chosen.example');
    expect(saved.bakedFor).toBe('https://vyzor.example');
  });

  it('survives unreadable storage rather than throwing', async () => {
    const { useConnectionStore } = await loadStore('https://vyzor.example', 'not json');
    expect(useConnectionStore.getState().baseUrl).toBe('https://vyzor.example');
  });
});
