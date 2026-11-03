/**
 * useFileBrowser — view mode, sorting and filtering for the file list.
 *
 * All three are view concerns, so they live here rather than in the query layer:
 * the server returns one ordered list and the UI decides how to present it.
 *
 * The settings themselves are held in a store, not component state, so switching
 * away from the library and back does not reset them. This hook is the adapter:
 * it reads the store and applies the settings to the list.
 */

import { useMemo } from 'react';
import { useFileBrowserStore } from '../../stores/file-browser-store';
import type {
  SortDirection,
  SortKey,
  StateFilter,
  TypeFilter,
  ViewMode,
} from '../../stores/file-browser-store';
import type { DownloadView } from '../../hooks/view-models';

export type { ViewMode, SortKey, SortDirection, TypeFilter, StateFilter };

export interface FileBrowserState {
  view: ViewMode;
  setView: (v: ViewMode) => void;
  sortKey: SortKey;
  sortDirection: SortDirection;
  setSort: (key: SortKey) => void;
  typeFilter: TypeFilter;
  setTypeFilter: (f: TypeFilter) => void;
  stateFilter: StateFilter;
  setStateFilter: (f: StateFilter) => void;
  /** Filters applied, then sorted. */
  visible: DownloadView[];
  activeFilterCount: number;
}

function compare(a: DownloadView, b: DownloadView, key: SortKey): number {
  switch (key) {
    case 'size':
      return a.bytes - b.bytes;
    case 'date':
      return a.createdAt - b.createdAt;
    case 'type':
      return a.kindLabel.localeCompare(b.kindLabel) || a.name.localeCompare(b.name);
    case 'name':
    default:
      return a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
  }
}

export function useFileBrowser(files: readonly DownloadView[]): FileBrowserState {
  const view = useFileBrowserStore((s) => s.view);
  const sortKey = useFileBrowserStore((s) => s.sortKey);
  const sortDirection = useFileBrowserStore((s) => s.sortDirection);
  const typeFilter = useFileBrowserStore((s) => s.typeFilter);
  const stateFilter = useFileBrowserStore((s) => s.stateFilter);
  const setView = useFileBrowserStore((s) => s.setView);
  const setSort = useFileBrowserStore((s) => s.setSort);
  const setTypeFilter = useFileBrowserStore((s) => s.setTypeFilter);
  const setStateFilter = useFileBrowserStore((s) => s.setStateFilter);

  const visible = useMemo(() => {
    const byType = (f: DownloadView) => {
      if (typeFilter === 'all') return true;
      if (typeFilter === 'video') return f.isVideo;
      if (typeFilter === 'audio') return f.isAudio;
      return f.isImage;
    };
    const byState = (f: DownloadView) => {
      if (stateFilter === 'all') return true;
      if (stateFilter === 'ongoing') return f.isActive;
      if (stateFilter === 'done') return f.state === 'done';
      return f.state === 'failed' || f.state === 'cancelled';
    };

    const filtered = files.filter((f) => byType(f) && byState(f));
    const sorted = [...filtered].sort((a, b) => compare(a, b, sortKey));
    if (sortDirection === 'desc') sorted.reverse();
    return sorted;
  }, [files, typeFilter, stateFilter, sortKey, sortDirection]);

  const activeFilterCount =
    (typeFilter !== 'all' ? 1 : 0) + (stateFilter !== 'all' ? 1 : 0);

  return {
    view,
    setView,
    sortKey,
    sortDirection,
    setSort,
    typeFilter,
    setTypeFilter,
    stateFilter,
    setStateFilter,
    visible,
    activeFilterCount,
  };
}
