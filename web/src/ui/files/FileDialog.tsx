/**
 * FileDialog — one small modal used for both rename and move.
 *
 * Rename and move are the same shape of task (type a value, confirm), so they
 * share a component rather than duplicating focus handling, Escape-to-close and
 * validation. The caller supplies the label, the hint and the suggestion chips.
 *
 * Focus is moved into the field on open and returned to the trigger on close,
 * so a keyboard user is never dropped back at the top of the document.
 */

import { useEffect, useId, useRef, useState } from 'react';
import { Button } from '../primitives/Button';

export interface FileDialogProps {
  title: string;
  label: string;
  hint?: string;
  initialValue: string;
  confirmLabel: string;
  /** Offered as one-tap choices, e.g. folder names. */
  suggestions?: readonly string[];
  busy?: boolean;
  error?: string | null;
  onConfirm: (value: string) => void;
  onClose: () => void;
}

export function FileDialog({
  title,
  label,
  hint,
  initialValue,
  confirmLabel,
  suggestions,
  busy = false,
  error = null,
  onConfirm,
  onClose,
}: FileDialogProps) {
  const [value, setValue] = useState(initialValue);
  const inputRef = useRef<HTMLInputElement>(null);
  const restoreTo = useRef<Element | null>(null);
  const fieldId = useId();

  useEffect(() => {
    restoreTo.current = document.activeElement;
    inputRef.current?.focus();
    inputRef.current?.select();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose();
    };
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('keydown', onKey);
      const back = restoreTo.current;
      if (back instanceof HTMLElement) back.focus();
    };
  }, [onClose]);

  const trimmed = value.trim();
  const canConfirm = trimmed.length > 0 && !busy;

  const submit = () => {
    if (canConfirm) onConfirm(trimmed);
  };

  return (
    <div className="fdialog__scrim" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div className="fdialog" role="dialog" aria-modal="true" aria-label={title}>
        <div className="fdialog__head">
          <h2 className="fdialog__title">{title}</h2>
        </div>
        <div className="fdialog__body">
          <label className="fdialog__label" htmlFor={fieldId}>
            {label}
          </label>
          <input
            id={fieldId}
            ref={inputRef}
            className="fdialog__input"
            value={value}
            spellCheck={false}
            autoComplete="off"
            onChange={(e) => setValue(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') submit();
            }}
          />
          {hint && <p className="fdialog__hint">{hint}</p>}
          {suggestions && suggestions.length > 0 && (
            <div className="fdialog__chips">
              {suggestions.map((s) => (
                <button
                  key={s}
                  type="button"
                  className={s === trimmed ? 'fdialog__chip is-on' : 'fdialog__chip'}
                  onClick={() => setValue(s)}
                >
                  {s}
                </button>
              ))}
            </div>
          )}
          {error && <p className="fdialog__error">{error}</p>}
        </div>
        <div className="fdialog__actions">
          <Button variant="ghost" onClick={onClose} disabled={busy}>
            Cancel
          </Button>
          <Button variant="primary" onClick={submit} disabled={!canConfirm} loading={busy}>
            {confirmLabel}
          </Button>
        </div>
      </div>
    </div>
  );
}
