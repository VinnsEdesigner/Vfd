/**
 * File-browser preferences: view mode, sort and filters.
 *
 * These are presentation preferences, not server data, and the user expects them
 * to stick: switching tabs must not reset how the library is arranged. So they
 * live in a store (which survives the view unmounting) and are persisted, so a
 * reload keeps them too.
 *
 * Only the preferences persist — never the selection, which is about specific
 * items and would be stale the moment anything changed on disk.
 */

import { create } from 'zustand';

const STORAGE_KEY = 'vyzorix.files.preferences';

export type ViewMode = 'grid' | 'list';
export type SortKey = 'name' | 'size' | 'date' | 'type';
export type SortDirection = 'asc' | 'desc';
export type TypeFilter = 'all' | 'video' | 'audio' | 'image';
export type StateFilter = 'all' | 'ongoing' | 'done' | 'problem';
/**
 * Where the bytes are. A separate axis from type and state, because "show me
 * only what is on this device" is a question about storage, not about media.
 */
export type LocationFilter = 'all' | 'device' | 'vyzor';

export interface FileBrowserPreferences {
  view: ViewMode;
  sortKey: SortKey;
  sortDirection: SortDirection;
  typeFilter: TypeFilter;
  stateFilter: StateFilter;
  locationFilter: LocationFilter;
}

export const DEFAULT_FILE_BROWSER: FileBrowserPreferences = {
  view: 'list',
  sortKey: 'date',
  sortDirection: 'desc',
  typeFilter: 'all',
  stateFilter: 'all',
  locationFilter: 'all',
};

const VIEWS: ReadonlySet<string> = new Set(['grid', 'list']);
const SORT_KEYS: ReadonlySet<string> = new Set(['name', 'size', 'date', 'type']);
const DIRECTIONS: ReadonlySet<string> = new Set(['asc', 'desc']);
const TYPE_FILTERS: ReadonlySet<string> = new Set(['all', 'video', 'audio', 'image']);
const STATE_FILTERS: ReadonlySet<string> = new Set(['all', 'ongoing', 'done', 'problem']);
const LOCATION_FILTERS: ReadonlySet<string> = new Set(['all', 'device', 'vyzor']);

/**
 * Read the persisted preferences, discarding anything that is not a value this
 * build understands — a stale or hand-edited entry must not break the view.
 */
export function loadFileBrowserPreferences(): FileBrowserPreferences {
  if (typeof localStorage === 'undefined') return { ...DEFAULT_FILE_BROWSER };
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return { ...DEFAULT_FILE_BROWSER };
    const p = JSON.parse(raw) as Partial<FileBrowserPreferences>;
    return {
      view: p.view && VIEWS.has(p.view) ? p.view : DEFAULT_FILE_BROWSER.view,
      sortKey: p.sortKey && SORT_KEYS.has(p.sortKey) ? p.sortKey : DEFAULT_FILE_BROWSER.sortKey,
      sortDirection:
        p.sortDirection && DIRECTIONS.has(p.sortDirection)
          ? p.sortDirection
          : DEFAULT_FILE_BROWSER.sortDirection,
      typeFilter:
        p.typeFilter && TYPE_FILTERS.has(p.typeFilter)
          ? p.typeFilter
          : DEFAULT_FILE_BROWSER.typeFilter,
      stateFilter:
        p.stateFilter && STATE_FILTERS.has(p.stateFilter)
          ? p.stateFilter
          : DEFAULT_FILE_BROWSER.stateFilter,
      locationFilter:
        p.locationFilter && LOCATION_FILTERS.has(p.locationFilter)
          ? p.locationFilter
          : DEFAULT_FILE_BROWSER.locationFilter,
    };
  } catch {
    return { ...DEFAULT_FILE_BROWSER };
  }
}

function persist(p: FileBrowserPreferences): void {
  if (typeof localStorage === 'undefined') return;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(p));
  } catch {
    /* storage unavailable; preferences stay for this session */
  }
}

export interface FileBrowserState extends FileBrowserPreferences {
  setView: (view: ViewMode) => void;
  /** Choosing the active key flips direction; a new key starts sensibly. */
  setSort: (key: SortKey) => void;
  setTypeFilter: (filter: TypeFilter) => void;
  setStateFilter: (filter: StateFilter) => void;
  setLocationFilter: (filter: LocationFilter) => void;
  clearFilters: () => void;
  reset: () => void;
}

export const useFileBrowserStore = create<FileBrowserState>((set, get) => ({
  ...loadFileBrowserPreferences(),
  setView: (view) => {
    set({ view });
    persist({ ...get(), view });
  },
  setSort: (key) => {
    const { sortKey, sortDirection } = get();
    const next: Partial<FileBrowserPreferences> =
      key === sortKey
        ? { sortDirection: sortDirection === 'asc' ? 'desc' : 'asc' }
        : { sortKey: key, sortDirection: key === 'size' || key === 'date' ? 'desc' : 'asc' };
    set(next);
    persist({ ...get(), ...next });
  },
  setTypeFilter: (typeFilter) => {
    set({ typeFilter });
    persist({ ...get(), typeFilter });
  },
  setStateFilter: (stateFilter) => {
    set({ stateFilter });
    persist({ ...get(), stateFilter });
  },
  setLocationFilter: (locationFilter) => {
    set({ locationFilter });
    persist({ ...get(), locationFilter });
  },
  clearFilters: () => {
    const next = {
      typeFilter: 'all' as const,
      stateFilter: 'all' as const,
      locationFilter: 'all' as const,
    };
    set(next);
    persist({ ...get(), ...next });
  },
  reset: () => {
    set({ ...DEFAULT_FILE_BROWSER });
    persist({ ...DEFAULT_FILE_BROWSER });
  },
}));
