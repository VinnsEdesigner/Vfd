/**
 * DownloadInfo — the live detail panel for one download.
 *
 * Opened from a download's breadcrumb menu, so the list can stay a list and the
 * numbers get room: percentage, transfer rate, remaining time, how many
 * connections are in use, the format that was chosen, and the transfer controls
 * (pause / resume / cancel).
 *
 * It reads the same view model the list does, so the two cannot disagree about
 * progress. Nothing here is estimated that can be measured: an unknown total
 * shows as "unknown", and a stalled transfer shows no rate rather than a stale
 * one.
 */

import { useEffect, useId, useRef } from 'react';
import { createPortal } from 'react-dom';
import { Button } from '../primitives/Button';
import { Tag } from '../primitives/Badge';
import { StatusBadge } from './StatusBadge';
import {
  IconPause,
  IconPlay,
  IconX,
  IconTrash,
  IconExternal,
  IconFolder,
  IconRefresh,
} from '../primitives/Icons';
import { formatRate, formatDuration, hostFromUrl, type DownloadView } from '../../hooks/view-models';

export interface DownloadInfoProps {
  download: DownloadView;
  /** Which action is in flight, so its button can spin. */
  busy?: string | null;
  onPause: () => void;
  onResume: () => void;
  onCancel: () => void;
  onOpen?: () => void;
  onReveal?: () => void;
  onDelete: () => void;
  onClose: () => void;
  /** Save the bytes onto this device. Absent when no tier is usable. */
  onSaveLocally?: () => void;
  /** Play the locally-stored copy. Absent until it is saved. */
  onPlayLocal?: () => void;
  /** True while the bytes are being written to this device. */
  saving?: boolean;
  /** 0..1 while saving. */
  saveProgress?: number;
  /** Already on this device. */
  savedLocally?: boolean;
  /** Why saving is not possible, when it is not. */
  localUnavailableReason?: string;
}

/** Seconds left at the current rate, or null when that cannot be known. */
function remainingSeconds(d: DownloadView): number | null {
  if (!d.isActive || d.bytesPerSecond <= 0 || d.bytes <= 0) return null;
  const left = d.bytes - d.bytes * d.fraction;
  if (left <= 0) return null;
  return left / d.bytesPerSecond;
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="dinfo__row">
      <span className="dinfo__k">{label}</span>
      <span className="dinfo__v">{children}</span>
    </div>
  );
}

export function DownloadInfo({
  download: d,
  busy = null,
  onPause,
  onResume,
  onCancel,
  onOpen,
  onReveal,
  onDelete,
  onClose,
  onSaveLocally,
  onPlayLocal,
  saving = false,
  saveProgress,
  savedLocally = false,
  localUnavailableReason,
}: DownloadInfoProps) {
  const closeRef = useRef<HTMLButtonElement>(null);
  const titleId = useId();
  const eta = remainingSeconds(d);
  const canPause = d.isActive;
  const canResume = d.isResumable;
  const canCancel = d.isActive || d.isResumable;

  useEffect(() => {
    closeRef.current?.focus();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        onClose();
      }
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  }, [onClose]);

  return createPortal(
    <div className="cdialog__scrim" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="dinfo" role="dialog" aria-modal="true" aria-labelledby={titleId}>
        <div className="dinfo__head">
          <div className="dinfo__titles">
            <h2 className="dinfo__title" id={titleId} title={d.name}>
              {d.name}
            </h2>
            <p className="dinfo__host" title={d.url}>
              {hostFromUrl(d.url)}
            </p>
          </div>
          <StatusBadge state={d.state} />
        </div>

        <div
          className="dinfo__bar"
          role="progressbar"
          aria-valuenow={d.percent}
          aria-valuemin={0}
          aria-valuemax={100}
          aria-label={`${d.name} progress`}
        >
          <span style={{ width: `${d.percent}%` }} />
        </div>

        <div className="dinfo__pct">
          <span className="dinfo__pct-value">{d.percent}%</span>
          <span className="dinfo__pct-sub">
            {d.downloadedLabel} of {d.bytes > 0 ? d.bytesLabel : 'unknown'}
          </span>
        </div>

        <div className="dinfo__grid">
          <Row label="Speed">
            {d.isActive && d.bytesPerSecond > 0 ? (
              <span className="dinfo__strong">{formatRate(d.bytesPerSecond)}</span>
            ) : (
              <span className="muted">{d.isResumable ? 'Paused' : '—'}</span>
            )}
          </Row>
          <Row label="Remaining">
            {eta !== null ? <span>{formatDuration(eta)}</span> : <span className="muted">—</span>}
          </Row>
          <Row label="Total size">
            {d.bytes > 0 ? d.bytesLabel : <span className="muted">unknown</span>}
          </Row>
          <Row label="Downloaded">{d.downloadedLabel}</Row>
          <Row label="Format">
            {d.requestedQuality ? <Tag>{d.requestedQuality}</Tag> : <Tag>{d.kindLabel}</Tag>}
          </Row>
          <Row label="Container">
            {d.resolutionLabel ? `${d.kindLabel} · ${d.resolutionLabel}` : d.kindLabel}
          </Row>
          <Row label="Connections">
            {d.connections > 0 ? String(d.connections) : <span className="muted">—</span>}
          </Row>
          <Row label="Added">{d.createdAtLabel}</Row>
        </div>

        {saving && (
          <div className="dinfo__saving">
            <div className="dinfo__saving-bar">
              <span style={{ width: `${Math.round((saveProgress ?? 0) * 100)}%` }} />
            </div>
            <span className="dinfo__saving-label">
              Saving to this device… {Math.round((saveProgress ?? 0) * 100)}%
            </span>
          </div>
        )}

        {d.error !== null && <p className="dinfo__error">{d.error}</p>}

        <div className="dinfo__actions">
          <div className="dinfo__actions-left">
            {savedLocally && onPlayLocal && (
              <Button size="sm" variant="primary" onClick={onPlayLocal}>
                <IconPlay /> Play offline
              </Button>
            )}
            {!savedLocally && onSaveLocally && d.isOpenable && (
              <Button size="sm" loading={saving} onClick={onSaveLocally}>
                <IconFolder /> Save to device
              </Button>
            )}
            {onOpen && d.isOpenable && (
              <Button size="sm" variant="ghost" onClick={onOpen}>
                <IconPlay /> Open
              </Button>
            )}
            {onReveal && d.isOpenable && (
              <Button size="sm" variant="ghost" onClick={onReveal}>
                <IconFolder /> Show in folder
              </Button>
            )}
          </div>
          <div className="dinfo__actions-right">
            {canPause && (
              <Button size="sm" loading={busy === 'pause'} onClick={onPause}>
                <IconPause /> Pause
              </Button>
            )}
            {canResume && (
              <Button size="sm" variant="primary" loading={busy === 'resume'} onClick={onResume}>
                <IconRefresh /> Resume
              </Button>
            )}
            {canCancel && (
              <Button size="sm" variant="ghost" loading={busy === 'cancel'} onClick={onCancel}>
                <IconX /> Cancel
              </Button>
            )}
            <Button size="sm" variant="ghost" onClick={onDelete} aria-label="Delete">
              <IconTrash /> Delete
            </Button>
            <Button ref={closeRef} size="sm" variant="ghost" onClick={onClose}>
              Close
            </Button>
          </div>
        </div>
        {localUnavailableReason !== undefined && !savedLocally && (
          <p className="dinfo__hint">{localUnavailableReason}</p>
        )}
      </div>
    </div>,
    document.body,
  );
}

export { IconExternal };
