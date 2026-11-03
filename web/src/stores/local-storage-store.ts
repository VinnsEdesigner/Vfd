/**
 * The user's chosen local storage, and what we know about this device.
 *
 * The choice is persisted; the *handle* is not. A directory handle cannot be
 * serialised to localStorage, and it must be re-granted with a user gesture
 * anyway, so the store remembers which tier was chosen and whether a directory
 * was ever picked — then the UI asks for it again when it is needed.
 *
 * The chosen tier is re-validated on load. A choice made in Chrome is not valid
 * in Firefox, and a stale value must not be trusted just because it is in
 * storage.
 */

import { create } from 'zustand';
import {
  detectStorageEnvironment,
  requestPersistentStorage,
  type StorageEnvironment,
  type StorageTier,
  type StorageTierId,
} from '../platform/storage-capabilities';
import {
  forgetDirectoryHandle,
  requestDirectoryPermission,
  restoreDirectory,
  saveDirectoryHandle,
  type DirectoryPermission,
} from '../platform/directory-handle-store';

const KEY = 'vyzorix.local-storage';

export interface LocalStorageChoice {
  tier: StorageTierId;
  /** The name of the directory the user picked, for display only. */
  directoryName: string | null;
}

interface PersistedShape {
  tier?: string;
  directoryName?: string | null;
}

function load(): LocalStorageChoice | null {
  if (typeof localStorage === 'undefined') return null;
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return null;
    const p = JSON.parse(raw) as PersistedShape;
    const known: StorageTierId[] = ['file-system-access', 'opfs', 'indexeddb', 'memory'];
    if (!p.tier || !known.includes(p.tier as StorageTierId)) return null;
    return {
      tier: p.tier as StorageTierId,
      directoryName: p.directoryName ?? null,
    };
  } catch {
    return null;
  }
}

function persist(choice: LocalStorageChoice | null): void {
  if (typeof localStorage === 'undefined') return;
  try {
    if (choice) localStorage.setItem(KEY, JSON.stringify(choice));
    else localStorage.removeItem(KEY);
  } catch {
    /* storage unavailable; the choice still applies for this session */
  }
}

export interface LocalStorageState {
  /** The live directory handle, when one is held. Restored from IndexedDB. */
  directory: FileSystemDirectoryHandle | null;
  /** What the browser says about our access to that directory right now. */
  directoryPermission: DirectoryPermission;
  choice: LocalStorageChoice | null;
  environment: StorageEnvironment | null;
  /** True until the first detection completes. */
  detecting: boolean;
  /** Set when a tier could not be used, so the UI can say why. */
  error: string | null;
  /** Re-read the browser's capabilities and usage figures. */
  refresh: () => Promise<void>;
  /** Record a choice and, for the folder tier, the handle behind it. */
  choose: (tier: StorageTierId, directory?: FileSystemDirectoryHandle) => void;
  /** Re-grant access to a remembered folder. Must come from a user gesture. */
  regrantDirectory: () => Promise<DirectoryPermission>;
  /** Attempt to restore a remembered folder and report what it found. */
  restore: () => Promise<void>;
  clear: () => void;
  /** Ask the browser to exempt this origin from eviction. */
  requestPersistence: () => Promise<boolean>;
  /** The tier actually in force, after validation against this device. */
  effectiveTier: () => StorageTierId;
  tierInfo: () => StorageTier | undefined;
}

export const useLocalStorageStore = create<LocalStorageState>((set, get) => ({
  directory: null,
  directoryPermission: 'unknown',
  choice: load(),
  environment: null,
  detecting: true,
  error: null,

  refresh: async () => {
    set({ detecting: true });
    try {
      const environment = await detectStorageEnvironment();
      set({ environment, detecting: false, error: null });
    } catch (e) {
      set({
        detecting: false,
        error: e instanceof Error ? e.message : 'Could not read this device’s storage options.',
      });
    }
  },

  choose: (tier, directory) => {
    const choice: LocalStorageChoice = {
      tier,
      directoryName: directory?.name ?? null,
    };
    persist(choice);
    set({
      choice,
      directory: directory ?? null,
      // A freshly picked folder is granted; a tier without one has no state.
      directoryPermission: directory ? 'granted' : 'unknown',
      error: null,
    });
    // Remembered so the folder does not have to be picked again next time.
    if (directory) void saveDirectoryHandle(directory);
  },

  restore: async () => {
    const { handle, permission } = await restoreDirectory();
    if (handle) set({ directory: handle, directoryPermission: permission });
  },

  regrantDirectory: async () => {
    const handle = get().directory;
    if (!handle) return 'unknown';
    // Called straight from a click handler: the browser requires a gesture.
    const permission = await requestDirectoryPermission(handle);
    set({ directoryPermission: permission });
    return permission;
  },

  clear: () => {
    persist(null);
    void forgetDirectoryHandle();
    set({ choice: null, directory: null, directoryPermission: 'unknown' });
  },

  requestPersistence: async () => {
    const granted = await requestPersistentStorage();
    await get().refresh();
    return granted;
  },

  effectiveTier: () => {
    const { choice, environment, directory, directoryPermission } = get();
    if (!choice) return 'memory';
    const tier = environment?.tiers.find((t) => t.id === choice.tier);
    // A stored choice that this device cannot honour falls back, and the UI is
    // expected to re-ask rather than write somewhere unexpected.
    if (tier && !tier.available) return 'memory';
    // The folder tier needs a handle the browser will actually let us write
    // through. Remembered-but-not-regranted is not usable yet.
    if (choice.tier === 'file-system-access') {
      if (!directory) return 'memory';
      if (directoryPermission !== 'granted') return 'memory';
    }
    return choice.tier;
  },

  tierInfo: () => {
    const { environment } = get();
    return environment?.tiers.find((t) => t.id === get().effectiveTier());
  },
}));
