/**
 * FileList — the row view of the library.
 *
 * A file manager's row, not a data table: a leading thumbnail or glyph so the
 * eye can scan by type, the name and its metadata, then the trailing facts and
 * the overflow menu. Rows share the grid's selection behaviour (hold to start,
 * tap to toggle, tap to open when nothing is selected) so switching views does
 * not change how you work.
 */

import { IconFile, IconImage, IconMusic, IconPlay, IconFolder, IconCloud, IconCheck } from '../primitives/Icons';
import { StatusBadge } from '../downloads/StatusBadge';
import { OverflowMenu, type MenuAction } from '../menus/OverflowMenu';
import { useLongPress } from './useLongPress';
import { selectionActivation } from './selectionActivation';
import type { Selection } from './useSelection';
import type { DownloadView } from '../../hooks/view-models';

export type StorageTarget = 'local' | 'cloud';

export interface FileListProps {
  files: ReadonlyArray<DownloadView>;
  target: StorageTarget;
  localPath: string;
  cloudPath: string;
  contentUrl: (id: string) => string;
  selection: Selection;
  selecting: boolean;
  /** Ids in display order, so shift-click can select a range. */
  orderedIds: readonly string[];
  onOpen: (id: string) => void;
  actionsFor: (file: DownloadView) => MenuAction[];
  empty?: React.ReactNode;
}

const THUMB_IMAGE = new Set(['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'avif']);
const THUMB_VIDEO = new Set(['mp4', 'm4v', 'webm', 'ogv']);

export function FileList({
  files,
  target,
  localPath,
  cloudPath,
  contentUrl,
  selection,
  selecting,
  orderedIds,
  onOpen,
  actionsFor,
  empty,
}: FileListProps) {
  const isLocal = target === 'local';
  const path = isLocal ? localPath : cloudPath;

  if (files.length === 0 && empty) return <>{empty}</>;

  return (
    <div className="flist" role="listbox" aria-multiselectable="true">
      {files.map((f) => (
        <FileRow
          key={f.id}
          file={f}
          path={path}
          isLocal={isLocal}
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

interface FileRowProps {
  file: DownloadView;
  path: string;
  isLocal: boolean;
  contentUrl: (id: string) => string;
  selected: boolean;
  selecting: boolean;
  orderedIds: readonly string[];
  selection: Selection;
  onOpen: (id: string) => void;
  actions: MenuAction[];
}

function FileRow({
  file,
  path,
  isLocal,
  contentUrl,
  selected,
  selecting,
  orderedIds,
  selection,
  onOpen,
  actions,
}: FileRowProps) {
  const longPress = useLongPress(selectionActivation(file.id, selection, orderedIds, selecting, onOpen));

  const canThumbImage = file.isImage && THUMB_IMAGE.has(file.extension) && file.state === 'done';
  const canThumbVideo = file.isVideo && THUMB_VIDEO.has(file.extension) && file.state === 'done';
  const glyph = file.isImage ? <IconImage /> : file.isAudio ? <IconMusic /> : <IconFile />;

  return (
    <div
      className={selected ? 'frow is-selected' : 'frow'}
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
      <span className={selected ? 'frow__check is-on' : 'frow__check'} aria-hidden="true">
        {selected && <IconCheck />}
      </span>

      <span className="frow__thumb">
        {canThumbImage && <img src={contentUrl(file.id)} alt="" loading="lazy" draggable={false} />}
        {canThumbVideo && (
          <>
            <video src={contentUrl(file.id)} preload="metadata" muted playsInline />
            <span className="frow__play">
              <IconPlay />
            </span>
          </>
        )}
        {!canThumbImage && !canThumbVideo && <span className="frow__glyph">{glyph}</span>}
      </span>

      <span className="frow__main">
        <span className="frow__name" title={file.name}>
          {file.name}
        </span>
        <span className="frow__sub">
          {file.kindLabel} · {file.bytesLabel} · {file.createdAtLabel}
        </span>
      </span>

      <span className="frow__loc" title={path}>
        {isLocal ? <IconFolder /> : <IconCloud />}
        <span className="frow__loc-path">{path}</span>
      </span>

      <span className="frow__status">
        <StatusBadge state={file.state} />
      </span>

      <span className="frow__menu" onClick={(e) => e.stopPropagation()}>
        <OverflowMenu actions={actions} label={`Actions for ${file.name}`} />
      </span>
    </div>
  );
}
