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
import { DocumentViewer } from './DocumentViewer';
import { TextViewer } from './TextViewer';
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
  /**
   * Converts a container the browser cannot play (MKV, AVI) into MP4. Only
   * offered for a Vyzor item; a device copy or a remote candidate has no
   * Vyzor to convert it.
   */
  onConvert?: () => void;
  /** True while a conversion is running, so the button becomes a status line. */
  converting?: boolean;
  /** Why a conversion failed, when it did. */
  conversionError?: string | null;
  /** The URL of the converted MP4, once Vyzor has produced it. */
  convertedUrl?: string | null;
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
  onConvert,
  converting = false,
  conversionError = null,
  convertedUrl = null,
}: FileViewerProps) {
  const capability = viewerFor(file);
  const [dims, setDims] = useState<{ width: number; height: number } | null>(null);
  const [playbackError, setPlaybackError] = useState<string | null>(null);
  /**
   * Once Vyzor has converted the file, the MP4 is what plays — the
   * original container still cannot be decoded here. This takes precedence over
   * the original capability, so the same viewer switches to the converted file
   * without the user reopening anything.
   */
  const convertedSrc = file.conversion.state === 'done' && convertedUrl ? convertedUrl : null;

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
      {file.conversion.state !== 'none' && (
        <div className="kv__row">
          <span className="kv__k">Conversion</span>
          <span className="kv__v">
            {file.conversion.state === 'done'
              ? 'Converted to MP4'
              : file.conversion.state === 'converting'
                ? 'Converting…'
                : `Failed${conversionError ? ` — ${conversionError}` : ''}`}
          </span>
        </div>
      )}
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

  if (convertedSrc) {
    // Vyzor produced an MP4 from the original container; play that.
    stage = <VideoPlayer src={convertedSrc} onError={setPlaybackError} />;
  } else if (!capability.playable) {
    const Glyph = file.isImage ? IconImage : file.isAudio ? IconMusic : IconFile;
    const canConvert = Boolean(onConvert) && file.isVideo;
    stage = (
      <div className="viewerfallback">
        <span className="viewerfallback__icon">
          <Glyph />
        </span>
        <h2 className="viewerfallback__title">{file.kindLabel} can’t be previewed here</h2>
        <p className="viewerfallback__text">{capability.reason}</p>
        <div className="viewerfallback__actions">
          {/* A video the browser cannot decode is exactly what a conversion is
              for, so the offer is the primary action, not a footnote. */}
          {canConvert && (
            <button
              type="button"
              className="btn btn--primary"
              onClick={onConvert}
              disabled={converting}
            >
              {converting ? 'Converting…' : 'Convert to MP4'}
            </button>
          )}
          <button type="button" className="btn" onClick={onOpenWith}>
            Open with…
          </button>
          <button type="button" className="btn" onClick={onShare}>
            Share
          </button>
          <a className="btn" href={downloadUrl} download={file.filename}>
            Download
          </a>
        </div>
        {converting && (
          <Note className="viewerfallback__note">
            Converting to MP4 — this plays here as soon as it is ready.
          </Note>
        )}
        {conversionError && (
          <Note className="viewerfallback__note viewerfallback__note--warn">
            Conversion failed: {conversionError}
          </Note>
        )}
        {!canConvert && file.isVideo && (
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
  } else if (capability.kind === 'document') {
    stage = <DocumentViewer src={src} title={file.name} openUrl={downloadUrl} />;
  } else if (capability.kind === 'text') {
    stage = <TextViewer src={src} title={file.name} />;
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
      downloadFilename={file.filename}
      onOpenWith={onOpenWith}
      onShare={onShare}
      onDelete={onDelete}
      stage={stage}
    >
      {info}
    </ViewerShell>
  );
}
