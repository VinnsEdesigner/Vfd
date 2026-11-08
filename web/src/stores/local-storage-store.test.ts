/**
 * The local-storage store, and the folder-permission bug this pins.
 *
 * A remembered folder has two halves that can fall out of step: the *choice* in
 * `localStorage` and the *handle* in IndexedDB. The choice survives almost
 * anything; the handle does not (a cleared profile, a purged IndexedDB, a save
 * that failed). When only the choice survived, "re-grant access" had no handle
 * to re-grant — so `requestPermission` was never called and the button did
 * nothing at all. No picker, no error, no change: a dead button.
 *
 * These tests pin both halves of the fix:
 *   - a stale choice is dropped on restore, so the app asks again;
 *   - re-granting falls back to the picker when the handle is gone;
 *   - a real picker error is surfaced, while a user cancel stays silent.
 */

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const KEY = 'vyzorix.local-storage';

/** A minimal localStorage; jsdom's is per-file and we replace it. */
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
 * Load the store fresh, with a seeded choice and a stubbed IndexedDB.
 *
 * `storedHandle` is what `restoreDirectory` finds: null means "the choice
 * survived but the handle did not", which is the case under test.
 */
async function loadStore(opts: {
  seed?: { tier: string; directoryName: string | null };
  storedHandle?: { name: string; state?: PermissionState } | null;
}) {
  vi.resetModules();
  vi.stubGlobal('localStorage', fakeStorage());
  if (opts.seed) localStorage.setItem(KEY, JSON.stringify(opts.seed));

  // The handle store lives in IndexedDB, which jsdom does not implement. It is
  // not the subject here, so it is stubbed at the module boundary: what matters
  // is what the *store* does with the answer.
  vi.doMock('../platform/directory-handle-store', () => ({
    saveDirectoryHandle: vi.fn(async () => {}),
    loadDirectoryHandle: vi.fn(async () => opts.storedHandle ?? null),
    forgetDirectoryHandle: vi.fn(async () => {}),
    queryDirectoryPermission: vi.fn(async (h: { state?: PermissionState }) => h.state ?? 'prompt'),
    requestDirectoryPermission: vi.fn(async (h: { state?: PermissionState }) => h.state ?? 'granted'),
    restoreDirectory: vi.fn(async () => {
      const h = opts.storedHandle ?? null;
      return { handle: h, permission: h ? (h.state ?? 'prompt') : 'unknown' };
    }),
  }));
  return import('./local-storage-store');
}

/** A directory handle stand-in with a controllable permission state. */
function handle(name: string, state: PermissionState = 'granted') {
  return {
    name,
    queryPermission: async () => state,
    requestPermission: async () => state,
  };
}

beforeEach(() => {
  vi.unstubAllEnvs();
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.unstubAllEnvs();
  vi.doUnmock('../platform/directory-handle-store');
  vi.restoreAllMocks();
});

describe('local storage store — folder permission', () => {
  it('drops a stale choice when the handle is gone, so the app asks again', async () => {
    // The bug: the choice outlived the handle, so the app believed a folder was
    // configured and never asked again — while having nothing to write through.
    const { useLocalStorageStore } = await loadStore({
      seed: { tier: 'file-system-access', directoryName: 'My Downloads' },
      storedHandle: null,
    });
    const s = useLocalStorageStore.getState();
    await s.restore();

    const after = useLocalStorageStore.getState();
    expect(after.choice).toBeNull();
    expect(after.directory).toBeNull();
    // And it is not left in localStorage to fool the next load either.
    expect(localStorage.getItem(KEY)).toBeNull();
  });

  it('keeps the choice when the handle is still there', async () => {
    const { useLocalStorageStore } = await loadStore({
      seed: { tier: 'file-system-access', directoryName: 'My Downloads' },
      storedHandle: handle('My Downloads', 'prompt'),
    });
    await useLocalStorageStore.getState().restore();

    const after = useLocalStorageStore.getState();
    expect(after.choice?.directoryName).toBe('My Downloads');
    expect(after.directory).not.toBeNull();
    // A handle that needs re-confirming is remembered, not discarded.
    expect(after.directoryPermission).toBe('prompt');
  });

  it('re-grants through the held handle without opening the picker', async () => {
    const { useLocalStorageStore } = await loadStore({
      seed: { tier: 'file-system-access', directoryName: 'My Downloads' },
      storedHandle: handle('My Downloads', 'granted'),
    });
    await useLocalStorageStore.getState().restore();

    const picker = vi.fn(async () => handle('should-not-be-used'));
    vi.stubGlobal('window', { ...globalThis.window, showDirectoryPicker: picker });

    const permission = await useLocalStorageStore.getState().regrantOrRepick();
    expect(permission).toBe('granted');
    // The whole point: no picker when a handle is held.
    expect(picker).not.toHaveBeenCalled();
  });

  it('falls back to the picker when the handle is gone — the dead button', async () => {
    // The exact reported symptom: the choice says a folder exists, the handle
    // does not, and clicking re-grant did nothing whatsoever.
    const { useLocalStorageStore } = await loadStore({
      seed: { tier: 'file-system-access', directoryName: 'My Downloads' },
      storedHandle: null,
    });
    // `restore` drops the stale choice; a user then picks a folder from scratch.
    await useLocalStorageStore.getState().restore();

    const picked = handle('Chosen');
    const picker = vi.fn(async () => picked);
    vi.stubGlobal('window', { ...globalThis.window, showDirectoryPicker: picker });

    const permission = await useLocalStorageStore.getState().regrantOrRepick();
    expect(picker).toHaveBeenCalledTimes(1);
    expect(permission).toBe('granted');
    // And the new folder is remembered, so the next visit works.
    expect(useLocalStorageStore.getState().directory).toBe(picked);
    expect(JSON.parse(localStorage.getItem(KEY) ?? '{}').directoryName).toBe('Chosen');
  });

  it('reports a real picker failure rather than swallowing it', async () => {
    const { useLocalStorageStore } = await loadStore({ storedHandle: null });
    const picker = vi.fn(async () => {
      throw new DOMException('blocked by policy', 'NotAllowedError');
    });
    vi.stubGlobal('window', { ...globalThis.window, showDirectoryPicker: picker });

    // A non-abort error must reject, so the caller can show it. Swallowing every
    // error is how a picker that never opens looks like a dead button.
    await expect(useLocalStorageStore.getState().regrantOrRepick()).rejects.toThrow(
      'blocked by policy',
    );
  });

  it('treats the user cancelling the picker as a decision, not a failure', async () => {
    const { useLocalStorageStore } = await loadStore({ storedHandle: null });
    const picker = vi.fn(async () => {
      throw new DOMException('user aborted', 'AbortError');
    });
    vi.stubGlobal('window', { ...globalThis.window, showDirectoryPicker: picker });

    await expect(useLocalStorageStore.getState().regrantOrRepick()).resolves.toBe('unknown');
  });
});
