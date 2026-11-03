/**
 * DownloadTable — the downloads list, shared by Overview and Downloads.
 *
 * A download is a job, not a file, so each row leads with what the job produced
 * (name + source host) rather than a database id, and carries the live numbers
 * — progress, size, transfer rate — where the eye already is.
 *
 * The progress column shows a bar only while there is something to watch: a
 * finished row would just be a full bar repeated nine times, which is noise. It
 * reads "Done" instead, and the size column carries the real figure.
 */

import type { ReactNode } from 'react';
import { Tag } from '../primitives/Badge';
import { Button } from '../primitives/Button';
import { EmptyState } from '../primitives/EmptyState';
import { StatusBadge } from './StatusBadge';
import { OverflowMenu, type MenuAction } from '../menus/OverflowMenu';
import {
  IconFile,
  IconFolder,
  IconImage,
  IconInfo,
  IconMusic,
  IconPlay,
  IconTrash,
  IconX,
  IconExternal,
  IconShare,
} from '../primitives/Icons';
import {
  formatRate,
  hostFromUrl,
  type DownloadView,
} from '../../hooks/view-models';

export interface DownloadTableProps {
  downloads: ReadonlyArray<DownloadView>;
  /** Absolute URL of a finished item's bytes, for the row thumbnail. */
  contentUrl?: (id: string) => string;
  onOpen?: (id: string) => void;
  /** Hands a finished item to another app on the device. */
  onOpenWith?: (id: string) => void;
  /** Shares a finished item to another app or person. */
  onShare?: (id: string) => void;
  /** Stops an in-flight transfer but keeps the record. */
  onCancel?: (id: string) => void;
  /** Removes the record and its files. Destructive; the parent confirms. */
  onDelete?: (id: string) => void;
  /** Id whose cancel is in flight, so its action can show it. */
  cancellingId?: string | null;
  /** Opens the live detail panel for a download. */
  onInfo?: (id: string) => void;
  /** Saves a finished download onto this device. */
  onSaveLocally?: (id: string) => void;
  /** Play the locally-stored copy of a download. */
  onPlayLocal?: (id: string) => void;
  /** Ids already stored on this device. */
  storedIds?: ReadonlySet<string>;
  /** Ids currently being written to this device. */
  savingIds?: ReadonlySet<string>;
  emptyTitle?: string;
  emptyDescription?: string;
}

const THUMB_IMAGE = new Set(['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'avif']);
const THUMB_VIDEO = new Set(['mp4', 'm4v', 'webm', 'ogv']);

function glyphFor(d: DownloadView): ReactNode {
  if (d.isImage) return <IconImage />;
  if (d.isAudio) return <IconMusic />;
  return <IconFile />;
}

/** The leading tile: a real thumbnail once the bytes exist, a glyph before. */
function MediaTile({ d, contentUrl }: { d: DownloadView; contentUrl?: (id: string) => string }) {
  const canImage = Boolean(contentUrl) && d.isImage && THUMB_IMAGE.has(d.extension) && d.state === 'done';
  const canVideo = Boolean(contentUrl) && d.isVideo && THUMB_VIDEO.has(d.extension) && d.state === 'done';
  return (
    <span className="dlrow__tile">
      {canImage && <img src={contentUrl?.(d.id)} alt="" loading="lazy" draggable={false} />}
      {canVideo && (
        <>
          <video src={contentUrl?.(d.id)} preload="metadata" muted playsInline />
          <span className="dlrow__play">
            <IconPlay />
          </span>
        </>
      )}
      {!canImage && !canVideo && <span className="dlrow__glyph">{glyphFor(d)}</span>}
    </span>
  );
}

/** Progress: a live bar while transferring, an honest label otherwise. */
function ProgressCell({ d, onInfo }: { d: DownloadView; onInfo?: (id: string) => void }) {
  if (d.isActive || d.isResumable) {
    return (
      <button
        type="button"
        className="dlrow__progress"
        onClick={onInfo ? (e) => { e.stopPropagation(); onInfo(d.id); } : undefined}
        aria-label={`${d.name} progress — open download info`}
        tabIndex={onInfo ? 0 : -1}
      >
        <span
          className="dlrow__bar"
          role="progressbar"
          aria-valuenow={d.percent}
          aria-valuemin={0}
          aria-valuemax={100}
        >
          <span style={{ width: `${d.percent}%` }} />
        </span>
        <span className="dlrow__pct">{d.percent}%</span>
      </button>
    );
  }
  if (d.state === 'done') return <span className="dlrow__state muted">Done</span>;
  if (d.state === 'failed') return <span className="dlrow__state dlrow__state--bad">Stopped</span>;
  if (d.state === 'cancelled') return <span className="dlrow__state muted">Cancelled</span>;
  return <span className="dlrow__state muted">Queued</span>;
}

export function DownloadTable({
  downloads,
  contentUrl,
  onOpen,
  onOpenWith,
  onShare,
  onCancel,
  onDelete,
  cancellingId = null,
  onInfo,
  onSaveLocally,
  onPlayLocal,
  storedIds,
  savingIds,
  emptyTitle = 'No downloads yet',
  emptyDescription = 'Start a download to see it appear here.',
}: DownloadTableProps) {
  if (downloads.length === 0) {
    return <EmptyState title={emptyTitle} description={emptyDescription} />;
  }

  const actionsFor = (d: DownloadView): MenuAction[] => {
    const actions: MenuAction[] = [];
    // First, because it is what a running download is for: watching it.
    if (onInfo) {
      actions.push({
        id: 'info',
        label: 'Download info',
        icon: <IconInfo />,
        onSelect: () => onInfo(d.id),
      });
    }
    if (d.isOpenable && storedIds?.has(d.id) && onPlayLocal) {
      actions.push({
        id: 'play-local',
        label: 'Play offline',
        icon: <IconPlay />,
        onSelect: () => onPlayLocal(d.id),
      });
    }
    if (d.isOpenable && !storedIds?.has(d.id) && onSaveLocally) {
      actions.push({
        id: 'save-local',
        label: savingIds?.has(d.id) ? 'Saving to this device…' : 'Save to device',
        icon: <IconFolder />,
        disabled: savingIds?.has(d.id),
        onSelect: () => onSaveLocally(d.id),
      });
    }
    if (onOpen && d.isOpenable) {
      actions.push({
        id: 'open',
        label: d.isImage ? 'View' : 'Play',
        icon: d.isImage ? <IconImage /> : <IconPlay />,
        onSelect: () => onOpen(d.id),
      });
    }
    if (onOpenWith && d.isOpenable) {
      actions.push({
        id: 'open-with',
        label: 'Open with…',
        icon: <IconExternal />,
        onSelect: () => onOpenWith(d.id),
      });
    }
    if (onShare && d.isOpenable) {
      actions.push({
        id: 'share',
        label: 'Share',
        icon: <IconShare />,
        onSelect: () => onShare(d.id),
      });
    }
    if (onCancel && d.isActive) {
      actions.push({
        id: 'cancel',
        label: 'Cancel transfer',
        icon: <IconX />,
        onSelect: () => onCancel(d.id),
      });
    }
    if (onDelete) {
      actions.push({
        id: 'delete',
        label: 'Delete',
        icon: <IconTrash />,
        danger: true,
        onSelect: () => onDelete(d.id),
      });
    }
    return actions;
  };

  return (
    <div className="dlrows" role="list">
      {downloads.map((d) => {
        const host = hostFromUrl(d.url);
        const actions = actionsFor(d);
        return (
          <div
            key={d.id}
            className={d.isActive ? 'dlrow is-active' : d.isResumable ? 'dlrow is-paused' : 'dlrow'}
            role="listitem"
            tabIndex={0}
            aria-label={d.name}
            onClick={onOpen ? () => onOpen(d.id) : undefined}
            onKeyDown={
              onOpen
                ? (e) => {
                    if (e.key === 'Enter' || e.key === ' ') {
                      e.preventDefault();
                      onOpen(d.id);
                    }
                  }
                : undefined
            }
          >
            <MediaTile d={d} {...(contentUrl ? { contentUrl } : {})} />

            <span className="dlrow__main">
              <span className="dlrow__name" title={d.name}>
                {d.name}
              </span>
              <span className="dlrow__host" title={d.url}>
                {host}
              </span>
            </span>

            <span className="dlrow__status">
              <StatusBadge state={d.state} />
            </span>

            <span className="dlrow__progress-cell">
              <ProgressCell d={d} {...(onInfo ? { onInfo } : {})} />
            </span>

            <span className="dlrow__speed num">
              {d.isActive ? formatRate(d.bytesPerSecond) : '—'}
            </span>

            <span className="dlrow__size num">{d.bytesLabel}</span>

            <span className="dlrow__format">
              <Tag>{d.kindLabel}</Tag>
            </span>

            <span className="dlrow__actions" onClick={(e) => e.stopPropagation()}>
              {onOpen && d.isOpenable && (
                <Button size="sm" variant="ghost" onClick={() => onOpen(d.id)}>
                  {d.isImage ? <IconImage /> : <IconPlay />}
                  {d.isImage ? 'View' : 'Play'}
                </Button>
              )}
              {onCancel && d.isActive && (
                <Button
                  size="sm"
                  variant="ghost"
                  loading={cancellingId === d.id}
                  onClick={() => onCancel(d.id)}
                >
                  <IconX /> Cancel
                </Button>
              )}
              {actions.length > 0 && (
                <OverflowMenu actions={actions} label={`Actions for ${d.name}`} />
              )}
            </span>
          </div>
        );
      })}
    </div>
  );
}
