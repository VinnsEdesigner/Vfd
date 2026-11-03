/**
 * What this browser can actually do with local storage.
 *
 * The web has four meaningfully different ways to keep bytes on the device, and
 * they are not interchangeable:
 *
 *   file-system-access  a real directory the user picked, visible in Finder /
 *                       Explorer, no quota, survives eviction. Chromium desktop
 *                       only — and it needs a user gesture to (re)grant.
 *   opfs                the origin private file system: large, fast, sandboxed,
 *                       invisible to the user, and evictable.
 *   indexeddb           works everywhere, slower, quota-bound, and blobs are
 *                       copied in and out.
 *   memory              nothing persists; say so rather than pretending.
 *
 * This module only reports facts. It never picks for the user, and it never
 * claims a guarantee it cannot verify — `persisted` is read back from the
 * browser rather than assumed, because `persist()` is allowed to say no.
 */

/** How bytes can be kept on this device, best first. */
export type StorageTierId = 'file-system-access' | 'opfs' | 'indexeddb' | 'memory';

export interface StorageTraits {
  /** Survives a reload without asking again. */
  persistent: boolean;
  /** The user can find the files outside the browser. */
  userVisible: boolean;
  /** Practical for multi-gigabyte media. */
  largeFiles: boolean;
  /** Can write a growing file without buffering it all in memory. */
  streamable: boolean;
  /** The browser may delete it under pressure. */
  evictable: boolean;
}

export interface StorageTier {
  id: StorageTierId;
  label: string;
  /** One line on what it is, for the chooser. */
  summary: string;
  available: boolean;
  /** Plain-language reason when `available` is false. */
  unavailableReason?: string;
  traits: StorageTraits;
  /** Honest limits worth showing before the user commits. */
  caveats: string[];
}

export interface StorageEstimate {
  usage: number;
  quota: number;
  /** 0..1, or null when the browser gives no quota figure. */
  usedFraction: number | null;
}

export interface StorageEnvironment {
  /** Every tier we know about, with availability resolved. */
  tiers: StorageTier[];
  /** The best tier that is actually available. */
  recommended: StorageTierId | null;
  /** Browser-reported usage, or null when it does not report one. */
  estimate: StorageEstimate | null;
  /** Read back from the browser, not assumed. */
  persisted: boolean;
  /** Whether `persist()` could be offered. */
  canRequestPersistence: boolean;
  /** Running as an installed app. */
  standalone: boolean;
  /** Local storage APIs require a secure context. */
  secureContext: boolean;
  /** True when nothing on this device can hold bytes. */
  noneAvailable: boolean;
}

/* ------------------------------ detection ------------------------------ */

/** The optional parts of `navigator.storage` that are not in every lib. */
interface StorageManagerExtras {
  getDirectory?: () => Promise<unknown>;
  persist?: () => Promise<boolean>;
  persisted?: () => Promise<boolean>;
  estimate?: () => Promise<{ usage?: number; quota?: number }>;
}

type NavigatorWithExtras = Navigator & {
  storage?: StorageManager & StorageManagerExtras;
};

function nav(): NavigatorWithExtras | null {
  return typeof navigator === 'undefined' ? null : (navigator as NavigatorWithExtras);
}

/** `showDirectoryPicker` on the window: the local-disk half of the API. */
function hasDirectoryPicker(): boolean {
  if (typeof window === 'undefined') return false;
  const w = window as Window & { showDirectoryPicker?: unknown };
  return typeof w.showDirectoryPicker === 'function';
}

function hasOpfs(): boolean {
  const n = nav();
  return typeof n?.storage?.getDirectory === 'function';
}

function hasIndexedDb(): boolean {
  return typeof indexedDB !== 'undefined' && indexedDB !== null;
}

function isSecure(): boolean {
  if (typeof window === 'undefined') return false;
  // `isSecureContext` is the platform's own answer, so trust it over the scheme.
  return window.isSecureContext === true;
}

function isStandalone(): boolean {
  if (typeof window === 'undefined') return false;
  if (window.matchMedia?.('(display-mode: standalone)').matches) return true;
  // iOS Safari predates display-mode and uses a non-standard flag.
  const n = window.navigator as Navigator & { standalone?: boolean };
  return n.standalone === true;
}

/** Whether the storage APIs are gated on a secure context at all. */
function needsSecureContext(): boolean {
  return typeof window !== 'undefined' && 'isSecureContext' in window;
}

const TIERS: ReadonlyArray<Omit<StorageTier, 'available' | 'unavailableReason'>> = [
  {
    id: 'file-system-access',
    label: 'A folder on this computer',
    summary: 'Choose a folder and the files are written there for real, where your other apps can see them.',
    traits: {
      persistent: true,
      userVisible: true,
      largeFiles: true,
      streamable: true,
      evictable: false,
    },
    caveats: [
      'Chrome and Edge on desktop only.',
      'The browser asks for permission again each session.',
    ],
  },
  {
    id: 'opfs',
    label: "This app's private storage",
    summary: 'Fast, roomy, and kept by the browser for this app. Not visible in your file manager.',
    traits: {
      persistent: true,
      userVisible: false,
      largeFiles: true,
      streamable: true,
      evictable: true,
    },
    caveats: [
      'The browser may clear it if the device runs low on space.',
      'You cannot open these files from another app.',
    ],
  },
  {
    id: 'indexeddb',
    label: 'Browser database',
    summary: 'Works in every browser. Slower, and the whole file passes through memory.',
    traits: {
      persistent: true,
      userVisible: false,
      largeFiles: false,
      streamable: false,
      evictable: true,
    },
    caveats: [
      'Held to a smaller share of disk than the private storage.',
      'A very large file can be slow to write and read back.',
    ],
  },
  {
    id: 'memory',
    label: 'This session only',
    summary: 'Kept in memory until you close the tab. Nothing is written to disk.',
    traits: {
      persistent: false,
      userVisible: false,
      largeFiles: false,
      streamable: false,
      evictable: false,
    },
    caveats: ['Everything is lost when the tab closes or reloads.'],
  },
];

/**
 * Resolve which tiers this browser offers, and why the others do not.
 * Pure reporting: no side effects, no permission prompts.
 */
export function detectStorageTiers(): StorageTier[] {
  const secure = isSecure();
  const secureGated = needsSecureContext() && !secure;
  const picker = hasDirectoryPicker();
  const opfs = hasOpfs();
  const idb = hasIndexedDb();

  return TIERS.map((t) => {
    if (t.id === 'memory') return { ...t, available: true };
    if (secureGated) {
      return {
        ...t,
        available: false,
        unavailableReason: 'This page is not on a secure connection, so the browser blocks local storage.',
      };
    }
    if (t.id === 'file-system-access') {
      if (picker) return { ...t, available: true };
      return {
        ...t,
        available: false,
        unavailableReason:
          'This browser has no folder picker. Chrome or Edge on desktop can do this.',
      };
    }
    if (t.id === 'opfs') {
      if (opfs) return { ...t, available: true };
      return {
        ...t,
        available: false,
        unavailableReason: 'This browser has no private file system.',
      };
    }
    if (idb) return { ...t, available: true };
    return {
      ...t,
      available: false,
      unavailableReason: 'This browser has IndexedDB disabled (often in private mode).',
    };
  });
}

/** Browser-reported usage. Returns null when it will not say. */
export async function readStorageEstimate(): Promise<StorageEstimate | null> {
  const n = nav();
  if (typeof n?.storage?.estimate !== 'function') return null;
  try {
    const e = await n.storage.estimate();
    const usage = typeof e.usage === 'number' ? e.usage : 0;
    const quota = typeof e.quota === 'number' ? e.quota : 0;
    return {
      usage,
      quota,
      usedFraction: quota > 0 ? Math.min(Math.max(usage / quota, 0), 1) : null,
    };
  } catch {
    return null;
  }
}

/** Whether storage is already exempt from eviction, as the browser reports it. */
export async function isStoragePersisted(): Promise<boolean> {
  const n = nav();
  if (typeof n?.storage?.persisted !== 'function') return false;
  try {
    return await n.storage.persisted();
  } catch {
    return false;
  }
}

/**
 * Ask the browser to keep this origin's storage. It may refuse, so the result is
 * the browser's answer, not a success assumption.
 */
export async function requestPersistentStorage(): Promise<boolean> {
  const n = nav();
  if (typeof n?.storage?.persist !== 'function') return false;
  try {
    return await n.storage.persist();
  } catch {
    return false;
  }
}

/**
 * Everything the chooser needs, in one call. Read-only: it does not prompt for
 * permissions, so it is safe to run on load.
 */
export async function detectStorageEnvironment(): Promise<StorageEnvironment> {
  const tiers = detectStorageTiers();
  const available = tiers.filter((t) => t.available && t.id !== 'memory');
  const [estimate, persisted] = await Promise.all([
    readStorageEstimate(),
    isStoragePersisted(),
  ]);
  const n = nav();
  return {
    tiers,
    recommended: available[0]?.id ?? 'memory',
    estimate,
    persisted,
    canRequestPersistence: typeof n?.storage?.persist === 'function',
    standalone: isStandalone(),
    secureContext: isSecure(),
    noneAvailable: available.length === 0,
  };
}

/** A short, honest description of the chosen tier for the status line. */
export function describeTier(tier: StorageTier | undefined): string {
  if (!tier) return 'No storage chosen yet.';
  if (!tier.available) return `${tier.label} is not available here.`;
  return tier.summary;
}
