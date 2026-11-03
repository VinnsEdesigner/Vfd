/**
 * ViewerShell — the full-page chrome every viewer shares.
 *
 * A viewer takes over the content area rather than sitting in a dialog: a
 * video in a modal is cramped, and the dialog was only ever a placeholder.
 * The header keeps the filename, the actions and a way back.
 */

import type { ReactNode } from 'react';
import { Button } from '../primitives/Button';
import { IconX, IconExternal, IconShare, IconTrash } from '../primitives/Icons';

export interface ViewerShellProps {
  name: string;
  kindLabel: string;
  sizeLabel: string;
  onClose: () => void;
  /** The file's own URL, used for the real "Download" action. */
  downloadUrl: string;
  /** The name to save under, extension included. */
  downloadFilename: string;
  /** Hands the file to another app (share sheet / app chooser). */
  onOpenWith?: () => void;
  /** Shares the file to another app or person. */
  onShare?: () => void;
  onDelete?: () => void;
  /** Rendered under the media: metadata, notes, etc. */
  children: ReactNode;
  /** The media itself, centred in the stage. */
  stage: ReactNode;
}

export function ViewerShell({
  name,
  kindLabel,
  sizeLabel,
  onClose,
  downloadUrl,
  downloadFilename,
  onOpenWith,
  onShare,
  onDelete,
  stage,
  children,
}: ViewerShellProps) {
  return (
    <div className="viewerpage">
      <header className="viewerpage__bar">
        <Button variant="ghost" icon aria-label="Close viewer" onClick={onClose}>
          <IconX />
        </Button>
        <div className="viewerpage__ident">
          <span className="viewerpage__name" title={name}>
            {name}
          </span>
          <span className="viewerpage__meta">
            {kindLabel} · {sizeLabel}
          </span>
        </div>
        <div className="viewerpage__actions">
          {onOpenWith && (
            <Button size="sm" onClick={onOpenWith}>
              <IconExternal /> Open with
            </Button>
          )}
          {onShare && (
            <Button size="sm" variant="ghost" onClick={onShare}>
              <IconShare /> Share
            </Button>
          )}
          <a className="btn btn--sm" href={downloadUrl} download={downloadFilename}>
            Download
          </a>
          {onDelete && (
            <Button size="sm" variant="ghost" icon onClick={onDelete} aria-label="Delete file">
              <IconTrash />
            </Button>
          )}
        </div>
      </header>

      <div className="viewerpage__stage">{stage}</div>

      <div className="viewerpage__info">{children}</div>
    </div>
  );
}
