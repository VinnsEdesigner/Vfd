/**
 * The media HUD: everything the page has loaded, and what to do with it.
 *
 * WHAT IT IS
 *
 * An overlay on the live view, off until switched on. While it is on, the engine
 * reports what the page fetches, and this lists it — format, size, and for a
 * stream the qualities behind it — with a download or a remove for each.
 *
 * WHY IT IS AN OVERLAY RATHER THAN A PANEL
 *
 * The page underneath is the point of the view: a listing that pushed it aside
 * would make you choose between seeing the page and seeing what it offers. An
 * overlay keeps both, and can be dismissed without losing the session.
 *
 * WHAT IT DOES NOT DO
 *
 * It does not fetch anything. A download is handed to the engine, which is the
 * thing allowed to fetch on the user's behalf; this only says which URL. A
 * remove is local to the list, because "not interested in this one" is not a
 * statement about the page.
 */

import { useMemo, useState } from 'react';
import { Button } from '../primitives/Button';
import { Spinner } from '../primitives/Spinner';
import { IconX, IconDownload, IconImage, IconList } from '../primitives/Icons';
import {
  formatBandwidth,
  formatMediaSize,
  mediaFormatLabel,
  type LiveMediaItem,
  type LiveMediaVariant,
} from '../../hooks';

export interface MediaHudProps {
  /** Everything the page has loaded, newest first. */
  items: LiveMediaItem[];
  /** Whether the engine is collecting. Off means the list will not fill. */
  watching: boolean;
  /** A download for this URL has been asked for. */
  onDownload: (item: LiveMediaItem, variant?: LiveMediaVariant) => void;
  /** Forget this entry, for this viewing. */
  onRemove: (url: string) => void;
  /** Turn collection on or off. */
  onWatch: (on: boolean) => void;
  /** Ask the engine to re-read the page. */
  onRescan: () => void;
  /** Close the overlay. The session keeps running. */
  onClose: () => void;
  /** URLs already queued, so a row can say so rather than offering it twice. */
  queued?: ReadonlySet<string>;
}

/** One entry, with its variants when it is a manifest. */
function MediaRow({
  item,
  onDownload,
  onRemove,
  queued,
}: {
  item: LiveMediaItem;
  onDownload: (item: LiveMediaItem, variant?: LiveMediaVariant) => void;
  onRemove: (url: string) => void;
  queued?: ReadonlySet<string>;
}) {
  /** Which variant is chosen, for a manifest. The best is the default. */
  const [variant, setVariant] = useState<LiveMediaVariant | null>(
    item.variants.length > 0 ? item.variants[0]! : null,
  );
  const already = queued?.has(item.url) ?? false;

  return (
    <li className="hud__row">
      <div className="hud__row-main">
        <span className={`hud__kind hud__kind--${item.kind}`} aria-hidden="true">
          {item.kind === 'image' ? <IconImage /> : <IconList />}
        </span>
        <div className="hud__meta">
          <span className="hud__name" title={item.url}>
            {item.filename}
          </span>
          <span className="hud__tags">
            <span className="hud__tag">{mediaFormatLabel(item)}</span>
            <span className="hud__tag">{formatMediaSize(item.bytes)}</span>
            {item.mime ? <span className="hud__tag">{item.mime}</span> : null}
          </span>
        </div>
        <div className="hud__row-actions">
          <Button
            size="sm"
            variant="primary"
            onClick={() => onDownload(item, variant ?? undefined)}
            title={already ? 'Already queued' : 'Download this'}
          >
            <IconDownload /> {already ? 'Queued' : 'Download'}
          </Button>
          <button
            type="button"
            className="hud__remove"
            onClick={() => onRemove(item.url)}
            title="Remove from this list"
            aria-label={`Remove ${item.filename} from the list`}
          >
            <IconX />
          </button>
        </div>
      </div>

      {item.variants.length > 0 ? (
        <div className="hud__variants" role="radiogroup" aria-label="Quality">
          {item.variants.map((v) => (
            <button
              key={v.url}
              type="button"
              role="radio"
              aria-checked={variant?.url === v.url}
              className={variant?.url === v.url ? 'hud__variant is-active' : 'hud__variant'}
              onClick={() => setVariant(v)}
            >
              <span className="hud__variant-label">{v.label}</span>
              {v.bandwidth > 0 ? (
                <span className="hud__variant-rate">{formatBandwidth(v.bandwidth)}</span>
              ) : null}
            </button>
          ))}
        </div>
      ) : null}
    </li>
  );
}

export function MediaHud({
  items,
  watching,
  onDownload,
  onRemove,
  onWatch,
  onRescan,
  onClose,
  queued,
}: MediaHudProps) {
  /**
   * Whether images are shown.
   *
   * On by default, because the list is meant to be *everything* the page
   * offers — hiding a category would quietly make it not that. A gallery page
   * is mostly images, so the toggle is there for the case where they drown the
   * video someone is actually after.
   */
  const [hideImages, setHideImages] = useState(false);

  /**
   * Video and audio first, then manifests, then images.
   *
   * A page loads its icons and thumbnails before it loads anything worth
   * downloading, so a list in arrival order buries the thing someone wants under
   * a hundred decorations. Sorting by kind puts the media where it is read from.
   */
  const order: Record<string, number> = { video: 0, audio: 1, manifest: 2, image: 3 };
  const shown = useMemo(() => {
    const list = hideImages ? items.filter((i) => i.kind !== 'image') : items;
    return [...list].sort((a, b) => (order[a.kind] ?? 9) - (order[b.kind] ?? 9));
  }, [items, hideImages]);
  const imageCount = useMemo(() => items.filter((i) => i.kind === 'image').length, [items]);

  return (
    <div className="hud" role="dialog" aria-label="Media on this page">
      <div className="hud__bar">
        <strong className="hud__title">Media on this page</strong>
        <span className="hud__count">{shown.length}</span>
        <div className="hud__bar-actions">
          <Button
            size="sm"
            onClick={() => onWatch(!watching)}
            title={watching ? 'Stop collecting' : 'Start collecting'}
          >
            {watching ? 'Watching' : 'Watch'}
          </Button>
          <Button size="sm" onClick={onRescan} title="Re-read the page for media">
            Rescan
          </Button>
          <button
            type="button"
            className="hud__close"
            onClick={onClose}
            aria-label="Close the media list"
            title="Close"
          >
            <IconX />
          </button>
        </div>
      </div>

      {imageCount > 0 ? (
        <button type="button" className="hud__more" onClick={() => setHideImages(!hideImages)}>
          {hideImages ? `Show ${imageCount} image${imageCount === 1 ? '' : 's'}` : 'Hide images'}
        </button>
      ) : null}

      {shown.length === 0 ? (
        <div className="hud__empty">
          <Spinner />
          <p>
            {watching
              ? 'Nothing yet. Play something on the page, or press Rescan.'
              : 'Not collecting. Press Watch to list what this page loads.'}
          </p>
        </div>
      ) : (
        <ul className="hud__list">
          {shown.map((item) => (
            <MediaRow
              key={item.url}
              item={item}
              onDownload={onDownload}
              onRemove={onRemove}
              queued={queued}
            />
          ))}
        </ul>
      )}
    </div>
  );
}
