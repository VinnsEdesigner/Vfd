/**
 * Connection settings (server URL + token). Client-side state, so it lives in
 * a store rather than the query cache. Persisted to localStorage.
 *
 * No React here: stores stay framework-agnostic and are consumed by hooks.
 */

import { create } from 'zustand';

const STORAGE_KEY = 'vyzorix.media.connection';

export interface ConnectionSettings {
  baseUrl: string;
  authToken: string;
}

interface ConnectionState extends ConnectionSettings {
  setBaseUrl: (baseUrl: string) => void;
  setAuthToken: (token: string) => void;
  reset: () => void;
}

export const DEFAULT_CONNECTION: ConnectionSettings = {
  // An explicit build-time URL wins (a self-hosted or LAN deployment that knows
  // its Vyzor). Otherwise the same origin the page was served from, which is
  // what a reverse proxy in front of both the console and Vyzor gives you —
  // and which avoids a hardcoded localhost in a deployed build.
  baseUrl: import.meta.env.VITE_API_BASE_URL ?? (typeof location !== 'undefined' ? location.origin : ''),
  authToken: '',
};

/**
 * A saved connection, and the build-time default that was in force when it was
 * saved.
 *
 * The stamp is the whole point. A stored endpoint is only meaningful for the
 * build that stored it: a new deployment bakes in a new Vyzor address, and the
 * old one may point at a host that no longer exists. Without the stamp, the
 * stale value silently wins — which is how a console ends up talking to
 * nothing while insisting it is configured.
 */
interface StoredConnection extends ConnectionSettings {
  bakedFor?: string;
}

function load(): ConnectionSettings {
    if (typeof localStorage === 'undefined') return DEFAULT_CONNECTION;
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) return DEFAULT_CONNECTION;
        const parsed = JSON.parse(raw) as Partial<StoredConnection>;
        /*
         * Discard a saved endpoint that belongs to a different build.
         *
         * `bakedFor` is absent on anything saved before this field existed, and
         * an absent stamp means the value's provenance is unknown — so it is
         * treated as stale rather than trusted. That is the safe reading: the
         * cost of discarding a good saved endpoint is retyping an address, and
         * the cost of keeping a bad one is a console that cannot connect and
         * cannot explain why.
         */
        if (parsed.bakedFor !== DEFAULT_CONNECTION.baseUrl) {
            return DEFAULT_CONNECTION;
        }
        return {
            baseUrl: parsed.baseUrl ?? DEFAULT_CONNECTION.baseUrl,
            authToken: parsed.authToken ?? DEFAULT_CONNECTION.authToken,
        };
    } catch {
        return DEFAULT_CONNECTION;
    }
}

function persist(s: ConnectionSettings): void {
    if (typeof localStorage === 'undefined') return;
    try {
        const stamped: StoredConnection = { ...s, bakedFor: DEFAULT_CONNECTION.baseUrl };
        localStorage.setItem(STORAGE_KEY, JSON.stringify(stamped));
    } catch {
        /* storage unavailable; settings stay in memory */
    }
}

export const useConnectionStore = create<ConnectionState>((set, get) => ({
  ...load(),
  setBaseUrl: (baseUrl) => {
    set({ baseUrl });
    persist({ baseUrl, authToken: get().authToken });
  },
  setAuthToken: (authToken) => {
    set({ authToken });
    persist({ baseUrl: get().baseUrl, authToken });
  },
  reset: () => {
    set(DEFAULT_CONNECTION);
    persist(DEFAULT_CONNECTION);
  },
}));

export function connectionSettings(state: ConnectionState): ConnectionSettings {
  return { baseUrl: state.baseUrl, authToken: state.authToken };
}
