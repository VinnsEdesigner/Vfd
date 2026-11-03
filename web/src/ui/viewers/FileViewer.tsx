/**
 * FileViewer — picks the viewer for a file and takes over the page.
 *
 * Routes on the *detected media kind*, not the file extension, so a mislabelled
 * file still opens correctly. When the browser cannot decode the format it
 * says so plainly and offers "Open with" instead of showing a dead player.
 */

import { useState } from 'react';
import { ViewerShell } from './ViewerShell';
import { ImageViewer } from './ImageViewer';
import { VideoPlayer } from './VideoPlayer';
import { AudioPlayer } from './AudioPlayer';
import { viewerFor } from './format';
import { Note } from '../primitives/Note';
import { IconFile, IconImage, IconMusic } from '../primitives/Icons';
import type { DownloadView } from '../../hooks/view-models';

export interface FileViewerProps {
  file: DownloadView;
  src: string;
  /** The file's own URL, used for the real "Download" action. */
  downloadUrl: string;
  /** Path shown in the info block. */
  location: string;
  onClose: () => void;
  /** Hands the file to another app on the device (app chooser where available). */
  onOpenWith: () => void;
  /** Shares the file to another app or person. */
  onShare: () => void;
  onDelete: () => void;
}

export function FileViewer({
  file,
  src,
  downloadUrl,
  location,
  onClose,
  onOpenWith,
  onShare,
  onDelete,
}: FileViewerProps) {
  const capability = viewerFor(file);
  const [dims, setDims] = useState<{ width: number; height: number } | null>(null);
  const [playbackError, setPlaybackError] = useState<string | null>(null);

  const info = (
    <div className="kv">
      <div className="kv__row">
        <span className="kv__k">Format</span>
        <span className="kv__v">{file.kindLabel}</span>
      </div>
      <div className="kv__row">
        <span className="kv__k">Size</span>
        <span className="kv__v">{file.bytesLabel}</span>
      </div>
      {dims && (
        <div className="kv__row">
          <span className="kv__k">Dimensions</span>
          <span className="kv__v">
            {dims.width} × {dims.height}
          </span>
        </div>
      )}
      {file.sha256 && (
        <div className="kv__row">
          <span className="kv__k">SHA-256</span>
          <span className="kv__v">{file.sha256}</span>
        </div>
      )}
      <div className="kv__row">
        <span className="kv__k">Saved to</span>
        <span className="kv__v">{location}</span>
      </div>
      {playbackError && (
        <div className="kv__row">
          <span className="kv__k">Playback</span>
          <span className="kv__v" style={{ color: 'var(--state-fail-fg)' }}>
            {playbackError}
          </span>
        </div>
      )}
    </div>
  );

  let stage: React.ReactNode;

  if (!capability.playable) {
    const Glyph = file.isImage ? IconImage : file.isAudio ? IconMusic : IconFile;
    stage = (
      <div className="viewerfallback">
        <span className="viewerfallback__icon">
          <Glyph />
        </span>
        <h2 className="viewerfallback__title">{file.kindLabel} can’t be previewed here</h2>
        <p className="viewerfallback__text">{capability.reason}</p>
        <div className="viewerfallback__actions">
          <button type="button" className="btn btn--primary" onClick={onOpenWith}>
            Open with…
          </button>
          <button type="button" className="btn" onClick={onShare}>
            Share
          </button>
          <a className="btn" href={downloadUrl} download={file.name}>
            Download
          </a>
        </div>
        {file.isVideo && (
          <Note className="viewerfallback__note">
            Transcoding to HLS produces a ladder the browser can play anywhere — that is
            what the transcode step is for.
          </Note>
        )}
      </div>
    );
  } else if (capability.kind === 'image') {
    stage = <ImageViewer src={src} alt={file.name} onDimensions={setDims} />;
  } else if (capability.kind === 'video') {
    stage = <VideoPlayer src={src} onError={setPlaybackError} />;
  } else {
    stage = <AudioPlayer src={src} title={file.name} onError={setPlaybackError} />;
  }

  return (
    <ViewerShell
      name={file.name}
      kindLabel={file.kindLabel}
      sizeLabel={file.bytesLabel}
      onClose={onClose}
      downloadUrl={downloadUrl}
      onOpenWith={onOpenWith}
      onShare={onShare}
      onDelete={onDelete}
      stage={stage}
    >
      {info}
    </ViewerShell>
  );
}
