/**
 * ConfirmDialog — an in-app confirmation for destructive actions.
 *
 * `window.confirm` is blocking browser chrome: it cannot be themed, it cannot be
 * styled, and on some platforms it is suppressed entirely. For an irreversible
 * delete it also needs to show *what* is about to go, which a native prompt
 * cannot do beyond one line of text.
 *
 * The destructive action is never the default: focus lands on Cancel, and Escape
 * cancels, so a stray Enter cannot delete anything.
 */

import { useEffect, useId, useRef } from 'react';
import { createPortal } from 'react-dom';
import { Button } from './Button';

export interface ConfirmDialogProps {
  title: string;
  /** The consequence, in plain words. */
  message: string;
  /** What is affected — file names, a count. */
  details?: readonly string[];
  confirmLabel?: string;
  cancelLabel?: string;
  /** Styles the confirm button as destructive and marks the dialog as such. */
  destructive?: boolean;
  busy?: boolean;
  onConfirm: () => void;
  onCancel: () => void;
}

export function ConfirmDialog({
  title,
  message,
  details,
  confirmLabel = 'Confirm',
  cancelLabel = 'Cancel',
  destructive = false,
  busy = false,
  onConfirm,
  onCancel,
}: ConfirmDialogProps) {
  const cancelRef = useRef<HTMLButtonElement>(null);
  const titleId = useId();

  useEffect(() => {
    cancelRef.current?.focus();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        onCancel();
      }
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  }, [onCancel]);

  return createPortal(
    <div
      className="cdialog__scrim"
      onMouseDown={(e) => e.target === e.currentTarget && !busy && onCancel()}
    >
      <div
        className="cdialog"
        role="alertdialog"
        aria-modal="true"
        aria-labelledby={titleId}
      >
        <h2 className="cdialog__title" id={titleId}>
          {title}
        </h2>
        <p className="cdialog__message">{message}</p>
        {details && details.length > 0 && (
          <ul className="cdialog__details">
            {details.map((d) => (
              <li key={d}>{d}</li>
            ))}
          </ul>
        )}
        <div className="cdialog__actions">
          <Button ref={cancelRef} variant="ghost" onClick={onCancel} disabled={busy}>
            {cancelLabel}
          </Button>
          <Button
            variant={destructive ? 'danger' : 'primary'}
            onClick={onConfirm}
            loading={busy}
          >
            {confirmLabel}
          </Button>
        </div>
      </div>
    </div>,
    document.body,
  );
}
