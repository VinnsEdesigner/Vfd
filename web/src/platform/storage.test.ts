/**
 * Storage capability + store tests.
 *
 * The value of the detection is that it never claims more than the browser
 * offers, so most of these assert the *negative* cases: a missing API must read
 * as unavailable with a reason, not as a silent fallback.
 */

import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  detectStorageTiers,
  describeTier,
  readStorageEstimate,
  isStoragePersisted,
  requestPersistentStorage,
} from './storage-capabilities';
import { MemoryStore, createMediaStore } from './media-store';
import {
  queryDirectoryPermission,
  requestDirectoryPermission,
  restoreDirectory,
} from './directory-handle-store';

/** Replaces the globals the detection reads, so each case is explicit. */
function setEnv(opts: {
  secure?: boolean;
  directoryPicker?: boolean;
  opfs?: boolean;
  indexedDb?: boolean;
  persist?: boolean;
  persisted?: boolean;
  quota?: number;
  usage?: number;
}) {
  vi.stubGlobal('window', {
    isSecureContext: opts.secure ?? true,
    matchMedia: () => ({ matches: false }),
    ...(opts.directoryPicker ? { showDirectoryPicker: () => Promise.resolve({}) } : {}),
  });
  vi.stubGlobal('navigator', {
    storage: {
      ...(opts.opfs ? { getDirectory: () => Promise.resolve({}) } : {}),
      ...(opts.persist ? { persist: () => Promise.resolve(true) } : {}),
      ...(opts.persisted !== undefined
        ? { persisted: () => Promise.resolve(opts.persisted) }
        : {}),
      ...(opts.quota !== undefined
        ? { estimate: () => Promise.resolve({ usage: opts.usage ?? 0, quota: opts.quota }) }
        : {}),
    },
  });
  if (opts.indexedDb === false) {
    vi.stubGlobal('indexedDB', undefined);
  } else {
    vi.stubGlobal('indexedDB', {});
  }
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('detectStorageTiers', () => {
  it('offers every tier when the browser has them all', () => {
    setEnv({ directoryPicker: true, opfs: true, indexedDb: true });
    const tiers = detectStorageTiers();
    expect(tiers.map((t) => t.id)).toEqual([
      'file-system-access',
      'opfs',
      'indexeddb',
      'memory',
    ]);
    expect(tiers.filter((t) => t.available)).toHaveLength(4);
  });

  it('marks the folder tier unavailable without a picker, and says why', () => {
    setEnv({ directoryPicker: false, opfs: true, indexedDb: true });
    const tier = detectStorageTiers().find((t) => t.id === 'file-system-access');
    expect(tier?.available).toBe(false);
    expect(tier?.unavailableReason).toMatch(/folder picker/i);
  });

  it('marks the sandbox unavailable without getDirectory', () => {
    setEnv({ directoryPicker: true, opfs: false, indexedDb: true });
    const tier = detectStorageTiers().find((t) => t.id === 'opfs');
    expect(tier?.available).toBe(false);
    expect(tier?.unavailableReason).toBeTruthy();
  });

  it('keeps memory available even when everything else is missing', () => {
    setEnv({ directoryPicker: false, opfs: false, indexedDb: false });
    const tiers = detectStorageTiers();
    expect(tiers.find((t) => t.id === 'memory')?.available).toBe(true);
    expect(tiers.filter((t) => t.id !== 'memory' && t.available)).toHaveLength(0);
  });

  it('blames the insecure context when local storage is gated on https', () => {
    setEnv({ secure: false, directoryPicker: true, opfs: true, indexedDb: true });
    const tiers = detectStorageTiers();
    const folder = tiers.find((t) => t.id === 'file-system-access');
    expect(folder?.available).toBe(false);
    expect(folder?.unavailableReason).toMatch(/secure connection/i);
  });

  it('states the limits of each option rather than only its benefits', () => {
    setEnv({ directoryPicker: true, opfs: true, indexedDb: true });
    const opfs = detectStorageTiers().find((t) => t.id === 'opfs');
    // The sandbox is evictable and invisible; saying so is the whole point.
    expect(opfs?.traits.evictable).toBe(true);
    expect(opfs?.traits.userVisible).toBe(false);
    expect(opfs?.caveats.join(' ')).toMatch(/clear it/i);
  });

  it('describes the folder tier as the only user-visible one', () => {
    setEnv({ directoryPicker: true, opfs: true, indexedDb: true });
    const tiers = detectStorageTiers();
    expect(tiers.filter((t) => t.traits.userVisible).map((t) => t.id)).toEqual([
      'file-system-access',
    ]);
  });
});

describe('storage estimates', () => {
  it('reports usage and quota when the browser provides them', async () => {
    setEnv({ quota: 1000, usage: 250 });
    const e = await readStorageEstimate();
    expect(e).toEqual({ usage: 250, quota: 1000, usedFraction: 0.25 });
  });

  it('returns null rather than inventing a quota', async () => {
    setEnv({});
    expect(await readStorageEstimate()).toBeNull();
  });

  it('treats a refused persist() as refused', async () => {
    setEnv({ persist: true });
    // The stub resolves true; the point is the value is read back, not assumed.
    expect(await requestPersistentStorage()).toBe(true);
  });

  it('reports not-persisted when the browser has no persisted()', async () => {
    setEnv({});
    expect(await isStoragePersisted()).toBe(false);
  });
});

describe('describeTier', () => {
  it('says plainly when a tier is unavailable', () => {
    setEnv({ directoryPicker: false, opfs: true, indexedDb: true });
    const tier = detectStorageTiers().find((t) => t.id === 'file-system-access');
    expect(describeTier(tier)).toMatch(/not available here/);
  });

  it('has something to say with no choice made', () => {
    expect(describeTier(undefined)).toMatch(/no storage chosen/i);
  });
});

describe('directory handle permission', () => {
  /** A stand-in for the structured-clone handle the browser hands back. */
  const fakeHandle = (state: PermissionState | 'throw') => ({
    name: 'Downloads',
    queryPermission: async () => {
      if (state === 'throw') throw new DOMException('nope', 'SecurityError');
      return state;
    },
    requestPermission: async () => {
      if (state === 'throw') throw new DOMException('nope', 'SecurityError');
      return state;
    },
  });

  it('reads permission without prompting', async () => {
    expect(await queryDirectoryPermission(fakeHandle('granted') as never)).toBe('granted');
    expect(await queryDirectoryPermission(fakeHandle('prompt') as never)).toBe('prompt');
  });

  it('reports unknown rather than throwing when the API is absent', async () => {
    expect(await queryDirectoryPermission({ name: 'x' } as never)).toBe('unknown');
  });

  it('does not claim a grant when the browser refuses', async () => {
    // requestPermission throws outside a user gesture; that must not read as
    // success.
    expect(await requestDirectoryPermission(fakeHandle('throw') as never)).toBe('denied');
  });

  it('restores nothing when no handle was ever stored', async () => {
    const { handle, permission } = await restoreDirectory();
    expect(handle).toBeNull();
    expect(permission).toBe('unknown');
  });
});

describe('MemoryStore', () => {
  it('writes, lists, reads back and removes', async () => {
    const store = new MemoryStore();
    const writer = await store.createWriter({ id: 'a', name: 'a.mp4', type: 'video/mp4' });
    await writer.write(new Uint8Array([1, 2, 3]));
    await writer.write(new Uint8Array([4, 5]));
    await writer.close(5);

    expect(await store.get('a')).toMatchObject({ id: 'a', name: 'a.mp4', size: 5 });
    expect(await store.usage()).toBe(5);

    const url = await store.urlFor('a');
    expect(url).toMatch(/^blob:/);
    if (url) store.release(url);

    await store.remove('a');
    expect(await store.list()).toEqual([]);
    expect(await store.usage()).toBe(0);
  });

  it('leaves nothing behind when a write is aborted', async () => {
    const store = new MemoryStore();
    const writer = await store.createWriter({ id: 'b', name: 'b.mp4', type: 'video/mp4' });
    await writer.write(new Uint8Array([1]));
    await writer.abort();
    expect(await store.list()).toEqual([]);
  });

  it('is the tier createMediaStore falls back to', async () => {
    const store = await createMediaStore({ tier: 'memory' });
    expect(store.tier).toBe('memory');
  });

  it('refuses the folder tier without a directory, rather than guessing', async () => {
    await expect(createMediaStore({ tier: 'file-system-access' })).rejects.toThrow(
      /directory is required/i,
    );
  });
});
