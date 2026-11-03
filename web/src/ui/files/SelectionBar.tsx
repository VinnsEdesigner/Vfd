/**
 * SelectionBar — the contextual action bar shown while items are selected.
 *
 * Appears in place of the toolbar so the actions are unmissable. Actions are
 * supplied by the caller, which owns the actual requests.
 */

import { Button } from '../primitives/Button';
import { IconX, IconShare, IconTrash, IconMove } from '../primitives/Icons';

export interface SelectionBarProps {
  count: number;
  onClear: () => void;
  onShare: () => void;
  onDelete: () => void;
  onMove: () => void;
  onSelectAll: () => void;
  busy?: boolean;
}

export function SelectionBar({
  count,
  onClear,
  onShare,
  onDelete,
  onMove,
  onSelectAll,
  busy = false,
}: SelectionBarProps) {
  return (
    <div className="selbar" role="toolbar" aria-label={`${count} selected`}>
      <Button variant="ghost" icon aria-label="Clear selection" onClick={onClear}>
        <IconX />
      </Button>
      <span className="selbar__count">{count} selected</span>
      <Button size="sm" variant="ghost" onClick={onSelectAll}>
        Select all
      </Button>

      <span className="selbar__spacer" />

      <Button size="sm" loading={busy} onClick={onShare}>
        <IconShare /> Share
      </Button>
      <Button size="sm" onClick={onMove}>
        <IconMove /> Move
      </Button>
      <Button size="sm" variant="ghost" onClick={onDelete} aria-label="Delete selected">
        <IconTrash /> Delete
      </Button>
    </div>
  );
}
