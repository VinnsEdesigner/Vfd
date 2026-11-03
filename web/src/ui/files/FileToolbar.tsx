/**
 * FileToolbar — filter, sort and view controls for the file list.
 *
 * Every control here does something. A button that opens an empty menu is worse
 * than no button, so the filter menu is real and shows a count when active.
 */

import { useEffect, useRef, useState } from 'react';
import { Button } from '../primitives/Button';
import { IconFilter, IconSort, IconGrid, IconList, IconChevronDown, IconCheck } from '../primitives/Icons';
import type {
  FileBrowserState,
  LocationFilter,
  SortKey,
  StateFilter,
  TypeFilter,
} from './useFileBrowser';

export interface FileToolbarProps {
  browser: FileBrowserState;
  /** Total before filtering, for the "n of m" readout. */
  totalCount: number;
}

const TYPE_OPTIONS: ReadonlyArray<{ id: TypeFilter; label: string }> = [
  { id: 'all', label: 'All types' },
  { id: 'video', label: 'Video' },
  { id: 'audio', label: 'Audio' },
  { id: 'image', label: 'Images' },
];

const STATE_OPTIONS: ReadonlyArray<{ id: StateFilter; label: string }> = [
  { id: 'all', label: 'Any state' },
  { id: 'ongoing', label: 'Ongoing' },
  { id: 'done', label: 'Finished' },
  { id: 'problem', label: 'Failed' },
];

const LOCATION_OPTIONS: ReadonlyArray<{ id: LocationFilter; label: string }> = [
  { id: 'all', label: 'Anywhere' },
  { id: 'device', label: 'On this device' },
  { id: 'engine', label: 'On the engine' },
];

const SORT_OPTIONS: ReadonlyArray<{ id: SortKey; label: string }> = [
  { id: 'name', label: 'Name' },
  { id: 'size', label: 'Size' },
  { id: 'date', label: 'Date added' },
  { id: 'type', label: 'Type' },
];

/** Closes a popover on outside click or Escape. */
function useDismiss(open: boolean, close: () => void) {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) close();
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') close();
    };
    document.addEventListener('mousedown', onDown);
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('mousedown', onDown);
      document.removeEventListener('keydown', onKey);
    };
  }, [open, close]);
  return ref;
}

export function FileToolbar({ browser, totalCount }: FileToolbarProps) {
  const [filterOpen, setFilterOpen] = useState(false);
  const [sortOpen, setSortOpen] = useState(false);
  const filterRef = useDismiss(filterOpen, () => setFilterOpen(false));
  const sortRef = useDismiss(sortOpen, () => setSortOpen(false));

  const shown = browser.visible.length;

  return (
    <div className="ftoolbar">
      <div className="ftoolbar__group">
        <div className="fpop" ref={filterRef}>
          <Button
            size="sm"
            variant={browser.activeFilterCount > 0 ? 'primary' : 'default'}
            onClick={() => setFilterOpen((v) => !v)}
            aria-expanded={filterOpen}
          >
            <IconFilter />
            Filter
            {browser.activeFilterCount > 0 && (
              <span className="fpop__count">{browser.activeFilterCount}</span>
            )}
          </Button>
          {filterOpen && (
            <div className="fpop__menu fpop__menu--left" role="menu">
              <div className="fpop__label">Type</div>
              {TYPE_OPTIONS.map((o) => (
                <button
                  key={o.id}
                  type="button"
                  role="menuitemradio"
                  aria-checked={browser.typeFilter === o.id}
                  className="fpop__item"
                  onClick={() => browser.setTypeFilter(o.id)}
                >
                  <span className="fpop__tick">
                    {browser.typeFilter === o.id && <IconCheck />}
                  </span>
                  {o.label}
                </button>
              ))}
              <div className="fpop__sep" />
              <div className="fpop__label">State</div>
              {STATE_OPTIONS.map((o) => (
                <button
                  key={o.id}
                  type="button"
                  role="menuitemradio"
                  aria-checked={browser.stateFilter === o.id}
                  className="fpop__item"
                  onClick={() => browser.setStateFilter(o.id)}
                >
                  <span className="fpop__tick">
                    {browser.stateFilter === o.id && <IconCheck />}
                  </span>
                  {o.label}
                </button>
              ))}
              <div className="fpop__sep" />
              <div className="fpop__label">Stored</div>
              {LOCATION_OPTIONS.map((o) => (
                <button
                  key={o.id}
                  type="button"
                  role="menuitemradio"
                  aria-checked={browser.locationFilter === o.id}
                  className="fpop__item"
                  onClick={() => browser.setLocationFilter(o.id)}
                >
                  <span className="fpop__tick">
                    {browser.locationFilter === o.id && <IconCheck />}
                  </span>
                  {o.label}
                </button>
              ))}
              {browser.activeFilterCount > 0 && (
                <>
                  <div className="fpop__sep" />
                  <button
                    type="button"
                    className="fpop__item"
                    onClick={() => {
                      browser.setTypeFilter('all');
                      browser.setStateFilter('all');
                      browser.setLocationFilter('all');
                    }}
                  >
                    <span className="fpop__tick" />
                    Clear filters
                  </button>
                </>
              )}
            </div>
          )}
        </div>

        <div className="fpop" ref={sortRef}>
          <Button size="sm" onClick={() => setSortOpen((v) => !v)} aria-expanded={sortOpen}>
            <IconSort />
            Sort
            <span className="ftoolbar__hint">
              {SORT_OPTIONS.find((o) => o.id === browser.sortKey)?.label}
              {browser.sortDirection === 'asc' ? ' ↑' : ' ↓'}
            </span>
          </Button>
          {sortOpen && (
            <div className="fpop__menu fpop__menu--left" role="menu">
              {SORT_OPTIONS.map((o) => (
                <button
                  key={o.id}
                  type="button"
                  role="menuitemradio"
                  aria-checked={browser.sortKey === o.id}
                  className="fpop__item"
                  onClick={() => browser.setSort(o.id)}
                >
                  <span className="fpop__tick">
                    {browser.sortKey === o.id && <IconCheck />}
                  </span>
                  {o.label}
                  {browser.sortKey === o.id && (
                    <span className="fpop__dir">
                      {browser.sortDirection === 'asc' ? 'A→Z' : 'Z→A'}
                    </span>
                  )}
                </button>
              ))}
            </div>
          )}
        </div>
      </div>

      <span className="ftoolbar__count">
        {shown === totalCount ? `${totalCount} items` : `${shown} of ${totalCount}`}
      </span>

      <div className="fviewtoggle" role="group" aria-label="View">
        <button
          type="button"
          className={browser.view === 'list' ? 'fviewtoggle__btn is-active' : 'fviewtoggle__btn'}
          aria-label="List view"
          aria-pressed={browser.view === 'list'}
          onClick={() => browser.setView('list')}
        >
          <IconList />
        </button>
        <button
          type="button"
          className={browser.view === 'grid' ? 'fviewtoggle__btn is-active' : 'fviewtoggle__btn'}
          aria-label="Grid view"
          aria-pressed={browser.view === 'grid'}
          onClick={() => browser.setView('grid')}
        >
          <IconGrid />
        </button>
      </div>
    </div>
  );
}

export { IconChevronDown };
