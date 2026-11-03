/**
 * DownloadsPage — the full download list.
 *
 * The list is the shared `DownloadTable`, so this page and the Overview can
 * never drift. Header actions are real: Refresh re-fetches, and New download is
 * the primary path. (An Export button used to sit here and did nothing.)
 */

import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { IconRefresh, IconPlus } from '../primitives/Icons';
import { DownloadTable } from '../downloads/DownloadTable';
import type { DownloadView } from '../../hooks/view-models';

export interface DownloadsPageProps {
  downloads: ReadonlyArray<DownloadView>;
  /** Absolute URL of a finished item's bytes, for row thumbnails. */
  contentUrl?: (id: string) => string;
  onOpen: (id: string) => void;
  onCancel?: (id: string) => void;
  onDelete?: (id: string) => void;
  /** Opens the live detail panel for a download. */
  onInfo?: (id: string) => void;
  cancellingId?: string | null;
  onRefresh?: () => void;
  refreshing?: boolean;
  onNewDownload?: () => void;
}

export function DownloadsPage({
  downloads,
  contentUrl,
  onOpen,
  onCancel,
  onDelete,
  onInfo,
  cancellingId = null,
  onRefresh,
  refreshing = false,
  onNewDownload,
}: DownloadsPageProps) {
  const active = downloads.filter((d) => d.isActive).length;
  const subtitle =
    active > 0
      ? `${downloads.length} total · ${active} in flight`
      : `${downloads.length} total`;

  return (
    <Card
      title="All downloads"
      subtitle={subtitle}
      actions={
        <>
          {onRefresh && (
            <Button size="sm" variant="ghost" loading={refreshing} onClick={onRefresh}>
              <IconRefresh /> Refresh
            </Button>
          )}
          {onNewDownload && (
            <Button size="sm" variant="primary" onClick={onNewDownload}>
              <IconPlus /> New download
            </Button>
          )}
        </>
      }
      flush
    >
      <DownloadTable
        downloads={downloads}
        {...(contentUrl ? { contentUrl } : {})}
        onOpen={onOpen}
        {...(onCancel ? { onCancel } : {})}
        {...(onDelete ? { onDelete } : {})}
        {...(onInfo ? { onInfo } : {})}
        cancellingId={cancellingId}
      />
    </Card>
  );
}
