/**
 * DownloadCard — one in-progress download.
 *
 * The bar is pinned to the bottom edge of the card and carries its own readout:
 * percentage on the left, live transfer rate on the right. Keeping the numbers
 * inside the bar means they travel with the progress rather than floating above
 * it, which reads better when several cards stack.
 */

import { IconFile } from '../primitives/Icons';
import { Button } from '../primitives/Button';
import { formatDuration, formatRate, type DownloadView } from '../../hooks/view-models';

export interface DownloadCardProps {
  download: DownloadView;
  onSelect?: (id: string) => void;
  /** Offered for in-progress downloads; the parent owns the request. */
  onCancel?: (id: string) => void;
  /** True while this card's cancel request is in flight. */
  cancelling?: boolean;
}

export function DownloadCard({
  download,
  onSelect,
  onCancel,
  cancelling = false,
}: DownloadCardProps) {
  const { percent, fraction, isActive } = download;
  const rate = formatRate(download.bytesPerSecond);

  const remaining =
    download.bytesPerSecond > 0 && download.bytes > 0
      ? formatDuration((download.bytes - download.bytes * fraction) / download.bytesPerSecond)
      : null;

  const sub = isActive
    ? `${download.downloadedLabel} of ${download.bytesLabel}${remaining ? ` · ${remaining} left` : ''}`
    : `${download.downloadedLabel} of ${download.bytesLabel} · waiting to start`;

  return (
    <div
      className="dl"
      role={onSelect ? 'button' : undefined}
      tabIndex={onSelect ? 0 : undefined}
      onClick={onSelect ? () => onSelect(download.id) : undefined}
      onKeyDown={
        onSelect
          ? (e) => {
              if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                onSelect(download.id);
              }
            }
          : undefined
      }
    >
      <div className="dl__top">
        <span className="dl__icon">
          <IconFile />
        </span>
        <div className="dl__meta">
          <div className="dl__name" title={download.name}>
            {download.name}
          </div>
          <div className="dl__sub">{sub}</div>
        </div>
        <div className="dl__speed">
          <span className="dl__speed-value">{rate}</span>
          <span className="dl__speed-label">transfer</span>
        </div>
        {onCancel && (
          <Button
            size="sm"
            variant="ghost"
            loading={cancelling}
            onClick={(e) => {
              e.stopPropagation();
              onCancel(download.id);
            }}
            aria-label={`Cancel ${download.name}`}
          >
            Cancel
          </Button>
        )}
      </div>

      <div
        className="dl__bar"
        role="progressbar"
        aria-valuenow={percent}
        aria-valuemin={0}
        aria-valuemax={100}
        aria-label={`${download.name} progress`}
      >
        <div className="dl__bar-fill" style={{ width: `${percent}%` }} />
        <span className="dl__bar-pct">{percent}%</span>
        <span className="dl__bar-speed">{rate}</span>
      </div>
    </div>
  );
}
