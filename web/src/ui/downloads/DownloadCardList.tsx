/**
 * DownloadCardList — the stack of in-progress downloads.
 *
 * Owns the empty case so pages do not each re-implement it.
 */

import { DownloadCard } from './DownloadCard';
import type { DownloadView } from '../../hooks/view-models';

export interface DownloadCardListProps {
  downloads: ReadonlyArray<DownloadView>;
  onSelect?: (id: string) => void;
  onCancel?: (id: string) => void;
  /** Id of the download whose cancel is in flight. */
  cancellingId?: string | null;
  emptyText?: string;
}

export function DownloadCardList({
  downloads,
  onSelect,
  onCancel,
  cancellingId = null,
  emptyText = 'Nothing downloading right now.',
}: DownloadCardListProps) {
  if (downloads.length === 0) return <div className="files__none">{emptyText}</div>;
  return (
    <div className="dl-list">
      {downloads.map((d) => (
        <DownloadCard
          key={d.id}
          download={d}
          {...(onSelect ? { onSelect } : {})}
          {...(onCancel ? { onCancel } : {})}
          cancelling={cancellingId === d.id}
        />
      ))}
    </div>
  );
}
