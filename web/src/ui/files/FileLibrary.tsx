/**
 * FileLibrary — the whole file-browser surface: toolbar, selection and view.
 *
 * This is where the pieces meet. It owns the view concerns (filter, sort, grid
 * vs list) and the selection, and it renders whichever of the toolbar or the
 * selection bar is relevant. The destructive and outbound work (share, delete,
 * move) is delegated to the caller, which owns the requests and the toasts —
 * a component that fetches cannot be reasoned about in isolation.
 */

import { useEffect, useMemo, useState } from 'react';
import { EmptyState } from '../primitives/EmptyState';
import { FileGrid } from './FileGrid';
import { FileList, type StorageTarget } from './FileList';
import { FileToolbar } from './FileToolbar';
import { SelectionBar } from './SelectionBar';
import { FileDialog } from './FileDialog';
import { useFileBrowser } from './useFileBrowser';
import { useSelection } from './useSelection';
import type { MenuAction } from '../menus/OverflowMenu';
import type { DownloadView } from '../../hooks/view-models';

export interface FileLibraryProps {
  files: ReadonlyArray<DownloadView>;
  target: StorageTarget;
  localPath: string;
  cloudPath: string;
  contentUrl: (id: string) => string;
  onOpen: (id: string) => void;
  onShare: (files: readonly DownloadView[]) => void;
  onDelete: (ids: readonly string[]) => void;
  onMove: (ids: readonly string[], folder: string) => void;
  onRename?: (id: string, name: string) => void;
  /** A rename or move is in flight; the dialog's confirm button spins. */
  busy?: boolean;
  /** Folder names offered as one-tap choices when moving. */
  moveFolders?: readonly string[];
}

const DEFAULT_MOVE_FOLDERS = ['Videos', 'Images', 'Audio', 'Documents', 'Archive'] as const;

export function FileLibrary({
  files,
  target,
  localPath,
  cloudPath,
  contentUrl,
  onOpen,
  onShare,
  onDelete,
  onMove,
  onRename,
  busy = false,
  moveFolders = DEFAULT_MOVE_FOLDERS,
}: FileLibraryProps) {
  const browser = useFileBrowser(files);
  const selection = useSelection();
  const [moveOpen, setMoveOpen] = useState(false);
  const [renameTarget, setRenameTarget] = useState<DownloadView | null>(null);

  const visibleIds = useMemo(() => browser.visible.map((f) => f.id), [browser.visible]);

  // A deleted item must not linger in the selection, or the action bar would
  // offer to act on something that is gone.
  useEffect(() => {
    const present = new Set(files.map((f) => f.id));
    const gone = [...selection.ids].filter((id) => !present.has(id));
    if (gone.length > 0) selection.remove(gone);
  }, [files, selection]);

  const selectedFiles = useMemo(
    () => files.filter((f) => selection.isSelected(f.id)),
    [files, selection],
  );

  const actionsFor = (file: DownloadView): MenuAction[] => {
    const verb = file.isImage ? 'View' : 'Play';
    const actions: MenuAction[] = [];
    if (file.isOpenable) {
      actions.push({ id: 'open', label: verb, onSelect: () => onOpen(file.id) });
    }
    actions.push({
      id: 'select',
      label: selection.isSelected(file.id) ? 'Deselect' : 'Select',
      onSelect: () => selection.toggle(file.id),
    });
    actions.push({
      id: 'share',
      label: 'Share',
      onSelect: () => onShare([file]),
    });
    actions.push({
      id: 'move',
      label: 'Move to…',
      onSelect: () => {
        selection.selectOnly(file.id);
        setMoveOpen(true);
      },
    });
    if (onRename) {
      actions.push({
        id: 'rename',
        label: 'Rename…',
        onSelect: () => setRenameTarget(file),
      });
    }
    actions.push({
      id: 'delete',
      label: 'Delete',
      danger: true,
      onSelect: () => onDelete([file.id]),
    });
    return actions;
  };

  const empty = (
    <EmptyState
      title="No files here"
      description={
        browser.activeFilterCount > 0
          ? 'Nothing matches the current filters.'
          : 'Finished downloads will appear here.'
      }
    />
  );

  const selectedIds = [...selection.ids];

  return (
    <div className="flibrary">
      {selection.active ? (
        <SelectionBar
          count={selection.size}
          onClear={selection.clear}
          onSelectAll={() => selection.selectAll(visibleIds)}
          onShare={() => onShare(selectedFiles)}
          onDelete={() => onDelete(selectedIds)}
          onMove={() => setMoveOpen(true)}
        />
      ) : (
        <FileToolbar browser={browser} totalCount={files.length} />
      )}

      {browser.view === 'grid' ? (
        <FileGrid
          files={browser.visible}
          contentUrl={contentUrl}
          selection={selection}
          selecting={selection.active}
          orderedIds={visibleIds}
          onOpen={onOpen}
          actionsFor={actionsFor}
        />
      ) : (
        <FileList
          files={browser.visible}
          target={target}
          localPath={localPath}
          cloudPath={cloudPath}
          contentUrl={contentUrl}
          selection={selection}
          selecting={selection.active}
          orderedIds={visibleIds}
          onOpen={onOpen}
          actionsFor={actionsFor}
          empty={empty}
        />
      )}

      {moveOpen && (
        <FileDialog
          title={selection.size > 1 ? `Move ${selection.size} items` : 'Move item'}
          label="Folder"
          hint="A folder inside the library. The item keeps its name."
          initialValue={moveFolders[0] ?? 'Archive'}
          confirmLabel="Move"
          suggestions={moveFolders}
          busy={busy}
          onConfirm={(folder) => {
            onMove(selectedIds, folder);
            setMoveOpen(false);
          }}
          onClose={() => setMoveOpen(false)}
        />
      )}

      {renameTarget && (
        <FileDialog
          title="Rename"
          label="Name"
          hint="The file extension is kept automatically."
          initialValue={renameTarget.name}
          confirmLabel="Rename"
          busy={busy}
          onConfirm={(name) => {
            onRename?.(renameTarget.id, name);
            setRenameTarget(null);
          }}
          onClose={() => setRenameTarget(null)}
        />
      )}
    </div>
  );
}
