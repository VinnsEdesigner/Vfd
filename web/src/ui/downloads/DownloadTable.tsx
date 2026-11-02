/**
 * DownloadTable — the downloads list.
 *
 * Composes DataTable + StatusBadge + ProgressBar. Takes `DownloadView[]` (from
 * hooks), so it never touches the domain layer.
 */

import { DataTable, type Column } from '../primitives/DataTable';
import { ProgressBar } from '../primitives/ProgressBar';
import { Tag } from '../primitives/Badge';
import { EmptyState } from '../primitives/EmptyState';
import { StatusBadge } from './StatusBadge';
import { formatBytes, formatRate, type DownloadView } from '../../hooks/view-models';

export interface DownloadTableProps {
  downloads: ReadonlyArray<DownloadView>;
  onSelect?: (id: string) => void;
  emptyTitle?: string;
  emptyDescription?: string;
}

const columns: ReadonlyArray<Column<DownloadView>> = [
  {
    key: 'id',
    header: 'ID',
    width: '84px',
    // The full id is 32 chars; showing a prefix keeps the column readable and
    // the full value is still available on hover and in the detail drawer.
    render: (d) => (
      <span className="cell-id" title={d.id}>
        {d.id.slice(0, 8)}
      </span>
    ),
  },
  {
    key: 'source',
    header: 'Source',
    render: (d) => (
      <span className="cell-url" title={d.url}>
        {d.url}
      </span>
    ),
  },
  {
    key: 'status',
    header: 'Status',
    width: '120px',
    render: (d) => <StatusBadge state={d.state} />,
  },
  {
    key: 'progress',
    header: 'Progress',
    width: '150px',
    render: (d) => (
      <ProgressBar
        fraction={d.fraction}
        tone={d.state === 'done' ? 'done' : d.state === 'failed' ? 'failed' : 'running'}
      />
    ),
  },
  {
    key: 'size',
    header: 'Size',
    width: '88px',
    render: (d) => <span className="num">{d.bytesLabel}</span>,
  },
  {
    key: 'speed',
    header: 'Speed',
    width: '96px',
    render: (d) => <span className="num muted">{d.isActive ? formatRate(d.bytesPerSecond) : '—'}</span>,
  },
  {
    key: 'format',
    header: 'Format',
    width: '88px',
    render: (d) => <Tag>{d.kindLabel}</Tag>,
  },
];

export function DownloadTable({
  downloads,
  onSelect,
  emptyTitle = 'No downloads yet',
  emptyDescription = 'Start a download to see it appear here.',
}: DownloadTableProps) {
  return (
    <DataTable
      columns={columns}
      rows={downloads}
      rowKey={(d) => d.id}
      {...(onSelect ? { onRowClick: (d: DownloadView) => onSelect(d.id) } : {})}
      empty={<EmptyState title={emptyTitle} description={emptyDescription} />}
    />
  );
}

/** Kept next to the table so the two cannot drift. */
export { formatBytes };
