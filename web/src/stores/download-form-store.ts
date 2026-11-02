/**
 * Draft state for the "new download" form. Presentation state that should survive
 * navigation but is not server data, so it belongs in a store.
 */

import { create } from 'zustand';
import type { CreateDownloadInput } from '../domain/media';

export const DEFAULT_DOWNLOAD_DRAFT: CreateDownloadInput = {
  url: '',
  hls: true,
  upload: false,
  purge: false,
  connections: 8,
  durationSeconds: 0,
};

export interface DownloadFormState extends CreateDownloadInput {
  setField: <K extends keyof CreateDownloadInput>(key: K, value: CreateDownloadInput[K]) => void;
  reset: () => void;
}

export const useDownloadFormStore = create<DownloadFormState>((set) => ({
  ...DEFAULT_DOWNLOAD_DRAFT,
  setField: (key, value) => set({ [key]: value } as Partial<DownloadFormState>),
  reset: () => set({ ...DEFAULT_DOWNLOAD_DRAFT }),
}));
