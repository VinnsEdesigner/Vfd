/**
 * selectionActivation — the one rule for what a press on an item does.
 *
 * Both views share it so the grid and the rows can never drift:
 *   hold                -> start a selection with just this item;
 *   shift-tap           -> extend the selection from the anchor to this item;
 *   tap while selecting -> toggle this item;
 *   tap otherwise       -> open the item.
 */

import type { Selection } from './useSelection';

export function selectionActivation(
  id: string,
  selection: Selection,
  orderedIds: readonly string[],
  selecting: boolean,
  onOpen: (id: string) => void,
) {
  return {
    onLongPress: () => selection.selectOnly(id),
    onTap: (e: { shiftKey: boolean; metaKey: boolean; ctrlKey: boolean }) => {
      if (selecting && e.shiftKey && selection.anchor) {
        selection.selectRange(selection.anchor, id, orderedIds);
        return;
      }
      if (selecting || e.shiftKey || e.metaKey || e.ctrlKey) {
        selection.toggle(id);
        return;
      }
      onOpen(id);
    },
  };
}
