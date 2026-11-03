/**
 * useSelection — multi-select state for the file list.
 *
 * Kept as a hook rather than a store because it is view-local: it resets when
 * the list unmounts, which is what you want for a file browser.
 *
 * There is no separate "selection mode" flag: having anything selected IS the
 * mode. That is the behaviour every file manager has, and it means the two can
 * never disagree.
 *
 * `anchor` records the last plain-clicked id so shift-click can select a range.
 */

import { useCallback, useMemo, useState } from 'react';

export interface Selection {
  ids: ReadonlySet<string>;
  size: number;
  /** The last clicked id, the origin of a shift-click range. */
  anchor: string | null;
  /** True once anything is selected; drives the action bar and tap semantics. */
  active: boolean;
  isSelected: (id: string) => boolean;
  /** Tap on an item while a selection exists. */
  toggle: (id: string) => void;
  /** Press-and-hold: start a selection with just this item. */
  selectOnly: (id: string) => void;
  /** Shift-click: extend from the anchor to this item. */
  selectRange: (fromId: string, toId: string, allIds: readonly string[]) => void;
  selectAll: (allIds: readonly string[]) => void;
  /** Also resets the range anchor. */
  clear: () => void;
  /** Drop ids that no longer exist (e.g. after a delete). */
  remove: (ids: readonly string[]) => void;
}

export function useSelection(): Selection {
  const [ids, setIds] = useState<Set<string>>(() => new Set());
  const [anchor, setAnchor] = useState<string | null>(null);

  const isSelected = useCallback((id: string) => ids.has(id), [ids]);

  const toggle = useCallback((id: string) => {
    setIds((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
    setAnchor(id);
  }, []);

  const selectOnly = useCallback((id: string) => {
    setIds(new Set([id]));
    setAnchor(id);
  }, []);

  const selectRange = useCallback(
    (fromId: string, toId: string, allIds: readonly string[]) => {
      const a = allIds.indexOf(fromId);
      const b = allIds.indexOf(toId);
      if (a === -1 || b === -1) return;
      const [lo, hi] = a <= b ? [a, b] : [b, a];
      setIds((prev) => {
        const next = new Set(prev);
        for (let i = lo; i <= hi; i++) next.add(allIds[i]!);
        return next;
      });
      setAnchor(toId);
    },
    [],
  );

  const selectAll = useCallback((allIds: readonly string[]) => {
    setIds(new Set(allIds));
  }, []);

  const clear = useCallback(() => {
    setIds(new Set());
    setAnchor(null);
  }, []);

  const remove = useCallback((gone: readonly string[]) => {
    setIds((prev) => {
      const next = new Set(prev);
      gone.forEach((id) => next.delete(id));
      return next;
    });
  }, []);

  return useMemo(
    () => ({
      ids,
      size: ids.size,
      anchor,
      active: ids.size > 0,
      isSelected,
      toggle,
      selectOnly,
      selectRange,
      selectAll,
      clear,
      remove,
    }),
    [ids, anchor, isSelected, toggle, selectOnly, selectRange, selectAll, clear, remove],
  );
}

export { type Selection as SelectionModel };
