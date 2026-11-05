/**
 * OverviewPage — stats, recent downloads, and the live pipeline for the
 * download currently in flight.
 */

import { Card } from '../primitives/Card';
import { StatsGrid, StatCard } from '../primitives/StatCard';
import { Button } from '../primitives/Button';
import { Tag } from '../primitives/Badge';
import { AppIcon } from '../primitives/AppIcon';
import { AppIconDownload, AppIconStorage } from '../primitives/AppIcons';
import { DownloadTable } from '../downloads/DownloadTable';
import type { DownloadView } from '../../hooks/view-models';

export interface OverviewPageProps {
  downloads: ReadonlyArray<DownloadView>;
  activeCount: number;
  completedCount: number;
  failedCount: number;
  processedLabel: string;
  /** Real library size measured by Vyzor. */
  libraryLabel: string;
  libraryFiles: number;
  /** Real volume figures; null when Vyzor could not read them. */
  disk: { freeLabel: string; usedPercent: number; summary: string } | null;
  onViewAll: () => void;
  onSelect: (id: string) => void;
  onOpenWith?: (id: string) => void;
  onShare?: (id: string) => void;
  /** Absolute URL of a finished item's bytes, for row thumbnails. */
  contentUrl?: (id: string) => string;
}

/** Grouped by media type so the list stays readable as it grows. */
const SUPPORTED_FORMATS: ReadonlyArray<{ group: string; items: string[] }> = [
  { group: 'Video', items: ['MP4', 'MKV', 'MOV', 'AVI', 'FLV', 'WMV', 'Ogg', 'MPEG-TS', 'HLS', 'DASH'] },
  { group: 'Audio', items: ['MP3', 'AAC', 'FLAC', 'WAV', 'Ogg'] },
  { group: 'Image', items: ['JPEG', 'PNG', 'GIF', 'WebP', 'BMP', 'TIFF', 'AVIF', 'HEIC'] },
];

export function OverviewPage({
  downloads,
  activeCount,
  completedCount,
  failedCount,
  processedLabel,
  libraryLabel,
  libraryFiles,
  disk,
  onViewAll,
  onSelect,
  onOpenWith,
  onShare,
  contentUrl,
}: OverviewPageProps) {
  const [libraryValue, libraryUnit] = splitValueUnit(libraryLabel);

  return (
    <>
      <StatsGrid>
        <StatCard
          label="Active downloads"
          value={String(activeCount)}
          unit={activeCount === 1 ? 'running' : 'running'}
          footnote={activeCount === 0 ? 'nothing in flight' : 'transferring now'}
        />
        <StatCard
          label="Completed"
          value={String(completedCount)}
          unit="files"
          footnote="finished successfully"
        />
        <StatCard
          label="Failed"
          value={String(failedCount)}
          unit="files"
          trend={failedCount > 0 ? '!' : undefined}
          trendDirection="down"
          footnote={failedCount > 0 ? 'needs attention' : 'none'}
        />
        <StatCard
          label="Library size"
          value={libraryValue}
          unit={libraryUnit}
          footnote={`${libraryFiles} file${libraryFiles === 1 ? '' : 's'} on disk`}
        />
      </StatsGrid>

      <div className="grid-2">
        <Card
          icon={
            <AppIcon size="xl">
              <AppIconDownload />
            </AppIcon>
          }
          title="Recent downloads"
          subtitle={`Latest ${Math.min(6, downloads.length)} of ${downloads.length}`}
          actions={
            <Button size="sm" variant="ghost" onClick={onViewAll}>
              View all
            </Button>
          }
          flush
        >
          <DownloadTable
            downloads={downloads.slice(0, 6)}
            onOpen={onSelect}
            {...(onOpenWith ? { onOpenWith } : {})}
            {...(onShare ? { onShare } : {})}
            {...(contentUrl ? { contentUrl } : {})}
          />
        </Card>

        <div className="stack">
          <Card
            icon={
              <AppIcon size="xl">
                <AppIconStorage />
              </AppIcon>
            }
            title="Storage"
          >
            {disk ? (
              <>
                <div className="usage">
                  <div className="usage__row">
                    <span>Bytes transferred</span>
                    <span className="num">{processedLabel}</span>
                  </div>
                  <div className="usage__row">
                    <span>Free space</span>
                    <span className="num">{disk.freeLabel}</span>
                  </div>
                </div>
                <div className="usage__bar">
                  <span style={{ width: `${Math.round(disk.usedPercent * 100)}%` }} />
                </div>
                <div className="field__hint" style={{ marginTop: 8 }}>
                  {disk.summary}
                </div>
              </>
            ) : (
              <div className="files__none">Storage figures unavailable.</div>
            )}
          </Card>

          <Card title="Supported formats">
            <div className="formats">
              {SUPPORTED_FORMATS.map((group) => (
                <div className="formats__group" key={group.group}>
                  <span className="formats__label">{group.group}</span>
                  <div className="formats__tags">
                    {group.items.map((f) => (
                      <Tag key={f}>{f}</Tag>
                    ))}
                  </div>
                </div>
              ))}
            </div>
          </Card>
        </div>
      </div>
    </>
  );
}

/** "1.9 GB" -> ["1.9", "GB"], so the unit can be styled separately. */
function splitValueUnit(label: string): [string, string] {
  const idx = label.lastIndexOf(' ');
  if (idx === -1) return [label, ''];
  return [label.slice(0, idx), label.slice(idx + 1)];
}
