/**
 * FileTable — the downloaded-files library.
 *
 * Finished files get an "Open" action; anything else gets "Details". The
 * storage location column reflects where downloads are currently being saved,
 * so switching local/cloud in Settings is visible here immediately.
 */

import { DataTable, type Column } from '../primitives/DataTable';
import { Tag } from '../primitives/Badge';
import { EmptyState } from '../primitives/EmptyState';
import { Button } from '../primitives/Button';
import { IconFolder, IconCloud, IconPlay, IconTrash, IconExternal, IconMusic, IconImage } from '../primitives/Icons';
import { OverflowMenu, type MenuAction } from '../menus/OverflowMenu';
import { AppIcon } from '../primitives/AppIcon';
import { AppIconFile } from '../primitives/AppIcons';
import { StatusBadge } from '../downloads/StatusBadge';
import type { DownloadView } from '../../hooks/view-models';

export type StorageTarget = 'local' | 'cloud';

export interface FileTableProps {
  files: ReadonlyArray<DownloadView>;
  target: StorageTarget;
  localPath: string;
  cloudPath: string;
  onOpen?: (id: string) => void;
  onDetails?: (id: string) => void;
  /** Requested by the menu; the parent performs the destructive call. */
  onDelete?: (id: string) => void;
  emptyTitle?: string;
  emptyDescription?: string;
}

export function FileTable({
  files,
  target,
  localPath,
  cloudPath,
  onOpen,
  onDetails,
  onDelete,
  emptyTitle = 'No files here',
  emptyDescription = 'Try a different filter.',
}: FileTableProps) {
  const isLocal = target === 'local';
  const path = isLocal ? localPath : cloudPath;

  const columns: ReadonlyArray<Column<DownloadView>> = [
    {
      key: 'file',
      header: 'File',
      render: (d) => (
        <div className="file-cell">
          <AppIcon size="sm" flat>
            <AppIconFile />
          </AppIcon>
          <span className="file-cell__name" title={d.name}>
            {d.name}
          </span>
        </div>
      ),
    },
    {
      key: 'status',
      header: 'Status',
      width: '120px',
      render: (d) => <StatusBadge state={d.state} />,
    },
    {
      key: 'size',
      header: 'Size',
      width: '88px',
      render: (d) => <span className="num">{d.bytesLabel}</span>,
    },
    {
      key: 'location',
      header: 'Location',
      width: '210px',
      render: () => (
        <span className="loc">
          {isLocal ? <IconFolder /> : <IconCloud />}
          <span className="loc__path">{path}</span>
        </span>
      ),
    },
    {
      key: 'added',
      header: 'Added',
      width: '118px',
      render: (d) => <span className="num muted">{d.createdAtLabel}</span>,
    },
    {
      key: 'actions',
      header: <span className="sr-only">Actions</span>,
      width: '170px',
      className: 'td-actions',
      render: (d) => {
        const playVerb = d.isImage ? 'View' : d.isAudio ? 'Play' : 'Play';
        const PlayIcon = d.isImage ? IconImage : d.isAudio ? IconMusic : IconPlay;
        const actions: MenuAction[] = [];
        if (d.isOpenable) {
          actions.push({
            id: 'play',
            label: playVerb,
            icon: <PlayIcon />,
            onSelect: () => onOpen?.(d.id),
          });
        }
        actions.push({
          id: 'open-with',
          label: 'Open with…',
          icon: <IconExternal />,
          onSelect: () => onOpen?.(d.id),
        });
        actions.push({
          id: 'reveal',
          label: 'Show in folder',
          icon: <IconFolder />,
          onSelect: () => onDetails?.(d.id),
        });
        if (onDelete) {
          actions.push({
            id: 'delete',
            label: 'Delete',
            icon: <IconTrash />,
            danger: true,
            onSelect: () => onDelete(d.id),
          });
        }
        return (
          <>
            {d.isOpenable && (
              <Button size="sm" onClick={() => onOpen?.(d.id)}>
                <PlayIcon /> {playVerb}
              </Button>
            )}
            <OverflowMenu actions={actions} label={`Actions for ${d.name}`} />
          </>
        );
      },
    },
  ];

  return (
    <DataTable
      columns={columns}
      rows={files}
      rowKey={(d) => d.id}
      className="file-table"
      empty={<EmptyState title={emptyTitle} description={emptyDescription} />}
    />
  );
}

export { Tag };
