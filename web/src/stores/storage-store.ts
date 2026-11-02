/**
 * Storage credentials the user typed for uploads.
 *
 * Deliberately NOT persisted: these are secrets. They live in memory for the
 * session and are cleared on reload. The store keeps them out of the query
 * cache so they never end up in a serialised cache dump either.
 */

import { create } from 'zustand';
import type { StorageCredentials } from '../domain/media';

export interface StorageState extends StorageCredentials {
  setField: <K extends keyof StorageCredentials>(key: K, value: StorageCredentials[K]) => void;
  clear: () => void;
  isComplete: () => boolean;
}

const EMPTY: StorageCredentials = {
  endpoint: '',
  region: 'us-east-1',
  bucket: '',
  accessKey: '',
  secretKey: '',
  sessionToken: '',
  cdnBase: '',
  cfToken: '',
  cfZone: '',
  pathStyle: true,
};

export const useStorageStore = create<StorageState>((set, get) => ({
  ...EMPTY,
  setField: (key, value) => set({ [key]: value } as Partial<StorageState>),
  clear: () => set({ ...EMPTY }),
  isComplete: () => {
    const s = get();
    return Boolean(s.bucket && s.accessKey && s.secretKey);
  },
}));

/** Strip empty strings so we only send headers the user actually set. */
export function effectiveCredentials(state: StorageState): StorageCredentials {
  const out: StorageCredentials = {};
  if (state.endpoint) out.endpoint = state.endpoint;
  if (state.region) out.region = state.region;
  if (state.bucket) out.bucket = state.bucket;
  if (state.accessKey) out.accessKey = state.accessKey;
  if (state.secretKey) out.secretKey = state.secretKey;
  if (state.sessionToken) out.sessionToken = state.sessionToken;
  if (state.cdnBase) out.cdnBase = state.cdnBase;
  if (state.cfToken) out.cfToken = state.cfToken;
  if (state.cfZone) out.cfZone = state.cfZone;
  out.pathStyle = state.pathStyle ?? true;
  return out;
}

