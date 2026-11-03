/**
 * FormatChooser — pick which format to download before it starts.
 *
 * A manifest advertises several renditions, and a single file is one format, so
 * the chooser always shows a list: one row per option with its dimensions,
 * codecs and size. Nothing starts until a row is chosen, because the choice
 * changes how many bytes are fetched and there is no undo for a 400 MB grab.
 *
 * Sizes come from the manifest's own declared bandwidth (times the duration the
 * user supplied). When a size cannot be derived the row says so rather than
 * inventing one.
 */

import { useEffect, useId, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { Button } from '../primitives/Button';
import { Tag } from '../primitives/Badge';
import { IconCheck } from '../primitives/Icons';
import type { QualityOption } from '../../hooks/view-models';

/** One selectable format. A single file yields exactly one of these. */
export interface FormatChoice {
  /** Stable id; also what is sent to the engine as `quality`. */
  id: string;
  label: string;
  /** e.g. "1920×1080", or a container name for a single file. */
  detail: string;
  /** Human size, or "size unknown". */
  sizeLabel: string;
  /** True when the size was measured rather than derived from a manifest. */
  sizeIsReal: boolean;
  codecs?: string;
}

export interface FormatChooserProps {
  /** What is being downloaded, for the heading. */
  source: string;
  host: string;
  /** The detected container/kind, e.g. "HLS" or "MP4". */
  kind: string;
  formats: ReadonlyArray<FormatChoice>;
  /** Pre-selected format id. */
  initialId: string | null;
  busy?: boolean;
  onConfirm: (id: string) => void;
  onCancel: () => void;
}

export function FormatChooser({
  source,
  host,
  kind,
  formats,
  initialId,
  busy = false,
  onConfirm,
  onCancel,
}: FormatChooserProps) {
  const [selected, setSelected] = useState<string | null>(
    initialId ?? formats[0]?.id ?? null,
  );
  const firstRef = useRef<HTMLButtonElement>(null);
  const titleId = useId();

  useEffect(() => {
    firstRef.current?.focus();
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
      <div className="fchooser" role="dialog" aria-modal="true" aria-labelledby={titleId}>
        <div className="fchooser__head">
          <h2 className="fchooser__title" id={titleId}>
            Choose a format
          </h2>
          <p className="fchooser__source" title={source}>
            {source} <span className="fchooser__host">{host}</span>
          </p>
        </div>

        <div className="fchooser__list" role="radiogroup" aria-label="Download format">
          {formats.map((f, i) => {
            const active = f.id === selected;
            return (
              <button
                key={f.id}
                ref={i === 0 ? firstRef : undefined}
                type="button"
                role="radio"
                aria-checked={active}
                className={active ? 'fchooser__opt is-active' : 'fchooser__opt'}
                onClick={() => setSelected(f.id)}
                onDoubleClick={() => !busy && onConfirm(f.id)}
              >
                <span className="fchooser__tick">{active && <IconCheck />}</span>
                <span className="fchooser__label">{f.label}</span>
                <span className="fchooser__detail">{f.detail}</span>
                {f.codecs !== undefined && f.codecs !== '' && (
                  <span className="fchooser__codecs" title={f.codecs}>
                    {f.codecs}
                  </span>
                )}
                <span
                  className={
                    f.sizeIsReal ? 'fchooser__size' : 'fchooser__size fchooser__size--est'
                  }
                  title={f.sizeIsReal ? 'Measured size' : 'Derived from the manifest bandwidth'}
                >
                  {f.sizeLabel}
                </span>
              </button>
            );
          })}
        </div>

        <div className="fchooser__foot">
          <span className="fchooser__kind">
            <Tag>{kind}</Tag>
            {formats.length > 1 && (
              <span className="fchooser__count">{formats.length} formats</span>
            )}
          </span>
          <div className="fchooser__actions">
            <Button variant="ghost" onClick={onCancel} disabled={busy}>
              Cancel
            </Button>
            <Button
              variant="primary"
              loading={busy}
              disabled={selected === null}
              onClick={() => selected !== null && onConfirm(selected)}
            >
              Download
            </Button>
          </div>
        </div>
      </div>
    </div>,
    document.body,
  );
}

/** Turns probed renditions into chooser rows. */
export function formatsFromQualities(qualities: ReadonlyArray<QualityOption>): FormatChoice[] {
  return qualities.map((q) => ({
    id: q.label,
    label: q.label,
    detail: q.width > 0 && q.height > 0 ? `${q.width}×${q.height}` : '—',
    sizeLabel: q.sizeLabel,
    sizeIsReal: false,
    codecs: q.codecs,
  }));
}

/** The single format a non-manifest source has. */
export function formatFromFile(container: string, sizeLabel: string): FormatChoice[] {
  return [
    {
      id: 'source',
      label: 'Source',
      detail: container || 'file',
      sizeLabel,
      sizeIsReal: true,
    },
  ];
}
