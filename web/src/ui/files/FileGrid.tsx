/**
 * FileGrid — thumbnail grid, the way a file manager shows media.
 *
 * Images render the real file (the engine serves the bytes, so a thumbnail is
 * just a scaled-down <img> — no separate thumbnail pipeline needed yet). Video
 * shows a <video preload="metadata"> frame, which browsers render as a poster
 * without downloading the whole file. Audio and anything undecodable fall back
 * to a glyph tile, because a broken image is worse than an honest icon.
 *
 * Selection follows the platform convention: press-and-hold (or right-click)
 * starts a selection, then a tap toggles. With nothing selected a tap opens the
 * file, so browsing is never blocked by the selection machinery.
 */

import { IconFile, IconImage, IconMusic, IconPlay, IconCheck } from '../primitives/Icons';
import { StatusBadge } from '../downloads/StatusBadge';
import { OverflowMenu, type MenuAction } from '../menus/OverflowMenu';
import { useLongPress } from './useLongPress';
import { selectionActivation } from './selectionActivation';
import type { Selection } from './useSelection';
import type { DownloadView } from '../../hooks/view-models';

export interface FileGridProps {
  files: ReadonlyArray<DownloadView>;
  contentUrl: (id: string) => string;
  selection: Selection;
  /** True when anything is selected; a tap then toggles instead of opening. */
  selecting: boolean;
  /** Ids in display order, so shift-click can select a range. */
  orderedIds: readonly string[];
  onOpen: (id: string) => void;
  actionsFor: (file: DownloadView) => MenuAction[];
}

/** Formats the browser will actually decode into a thumbnail. */
const THUMB_IMAGE = new Set(['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'avif']);
const THUMB_VIDEO = new Set(['mp4', 'm4v', 'webm', 'ogv']);

interface FileCardProps {
  file: DownloadView;
  contentUrl: (id: string) => string;
  selected: boolean;
  selecting: boolean;
  orderedIds: readonly string[];
  selection: Selection;
  onOpen: (id: string) => void;
  actions: MenuAction[];
}

function FileCard({
  file,
  contentUrl,
  selected,
  selecting,
  orderedIds,
  selection,
  onOpen,
  actions,
}: FileCardProps) {
  // A hold starts a selection; a tap opens (or toggles / extends, once selecting).
  const longPress = useLongPress(selectionActivation(file.id, selection, orderedIds, selecting, onOpen));

  const canThumbImage = file.isImage && THUMB_IMAGE.has(file.extension) && file.state === 'done';
  const canThumbVideo = file.isVideo && THUMB_VIDEO.has(file.extension) && file.state === 'done';

  return (
    <div
      className={selected ? 'fcard is-selected' : 'fcard'}
      role="option"
      tabIndex={0}
      aria-selected={selected}
      aria-label={file.name}
      onKeyDown={(e) => {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          if (selecting) selection.toggle(file.id);
          else onOpen(file.id);
        }
      }}
      {...longPress}
    >
      <div className="fcard__thumb">
        {canThumbImage && (
          <img src={contentUrl(file.id)} alt="" loading="lazy" draggable={false} />
        )}
        {canThumbVideo && (
          <>
            {/* preload=metadata paints a frame without fetching the file */}
            <video src={contentUrl(file.id)} preload="metadata" muted playsInline />
            <span className="fcard__play">
              <IconPlay />
            </span>
          </>
        )}
        {!canThumbImage && !canThumbVideo && (
          <span className="fcard__glyph">
            {file.isImage ? <IconImage /> : file.isAudio ? <IconMusic /> : <IconFile />}
          </span>
        )}

        <span className="fcard__status">
          <StatusBadge state={file.state} />
        </span>

        <span className={selected ? 'fcard__check is-on' : 'fcard__check'} aria-hidden="true">
          {selected && <IconCheck />}
        </span>

        <span className="fcard__menu" onClick={(e) => e.stopPropagation()}>
          <OverflowMenu actions={actions} label={`Actions for ${file.name}`} />
        </span>
      </div>
      <div className="fcard__meta">
        <span className="fcard__name" title={file.name}>
          {file.name}
        </span>
        <span className="fcard__sub">
          {file.kindLabel} · {file.bytesLabel}
        </span>
      </div>
    </div>
  );
}

export function FileGrid({
  files,
  contentUrl,
  selection,
  selecting,
  orderedIds,
  onOpen,
  actionsFor,
}: FileGridProps) {
  return (
    <div className="fgrid" role="listbox" aria-multiselectable="true">
      {files.map((f) => (
        <FileCard
          key={f.id}
          file={f}
          contentUrl={contentUrl}
          selected={selection.isSelected(f.id)}
          selecting={selecting}
          orderedIds={orderedIds}
          selection={selection}
          onOpen={onOpen}
          actions={actionsFor(f)}
        />
      ))}
    </div>
  );
}
