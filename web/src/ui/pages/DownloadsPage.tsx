/**
 * DownloadsPage — the full download list.
 */

import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { DownloadTable } from '../downloads/DownloadTable';
import type { DownloadView } from '../../hooks/view-models';

export interface DownloadsPageProps {
  downloads: ReadonlyArray<DownloadView>;
  onSelect: (id: string) => void;
}

export function DownloadsPage({ downloads, onSelect }: DownloadsPageProps) {
  return (
    <Card
      title="All downloads"
      subtitle={`${downloads.length} total · polling every second`}
      actions={
        <div style={{ display: 'flex', gap: 8 }}>
          <Button size="sm">Filter</Button>
          <Button size="sm">Export</Button>
        </div>
      }
      flush
    >
      <DownloadTable downloads={downloads} onSelect={onSelect} />
    </Card>
  );
}
