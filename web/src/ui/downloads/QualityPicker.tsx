/**
 * QualityPicker — choose a rendition when the source is a manifest.
 *
 * Only rendered when the source actually advertises variants. Sizes come from
 * the manifest's own BANDWIDTH (times the duration we were told); when that is
 * not derivable the row says "size unknown" rather than showing a guess.
 *
 * A torrent reuses this picker, because the question is the same shape — "which
 * of these do you want, and how big is it" — but the answer is a *file* rather
 * than a rendition. `variant="file"` swaps the compact badge for a filename that
 * wraps, which is what a long torrent path needs; everything else, including the
 * look, is shared so the two read as one control.
 */

import type { QualityOption } from '../../hooks/view-models';

export interface QualityPickerProps {
  options: ReadonlyArray<QualityOption>;
  selectedHeight: number | null;
  onSelect: (height: number) => void;
  /**
   *   quality  a stream rendition: a short badge, e.g. "1080p" (the default).
   *   file     a torrent file: the name wraps, since a path is long.
   */
  variant?: 'quality' | 'file';
  /** What to say when there is nothing to choose. */
  emptyLabel?: string;
}

export function QualityPicker({
  options,
  selectedHeight,
  onSelect,
  variant = 'quality',
  emptyLabel,
}: QualityPickerProps) {
  if (options.length === 0) {
    return (
      <div className="quality__empty">
        {emptyLabel ??
          'No selectable renditions — this source is a single file, so it downloads as-is.'}
      </div>
    );
  }

  const isFile = variant === 'file';
  return (
    <div
      className={isFile ? 'quality quality--files' : 'quality'}
      role="radiogroup"
      aria-label={isFile ? 'Which files to download' : 'Download quality'}
    >
      {options.map((q) => {
        const active = q.height === selectedHeight;
        return (
          <button
            key={`${q.height}-${q.url}`}
            type="button"
            role="radio"
            aria-checked={active}
            className={active ? 'quality__opt is-active' : 'quality__opt'}
            onClick={() => onSelect(q.height)}
          >
            {isFile ? (
              <span className="quality__file" title={q.label}>
                {q.label}
              </span>
            ) : (
              <span className="quality__badge">{q.label}</span>
            )}
            {!isFile && (
              <span className="quality__dims">
                {q.width > 0 && q.height > 0 ? `${q.width}×${q.height}` : '—'}
              </span>
            )}
            {!isFile && q.codecs !== '' && <span className="quality__codecs">{q.codecs}</span>}
            <span className={q.bytes >= 0 ? 'quality__size' : 'quality__size quality__size--unknown'}>
              {q.sizeLabel}
            </span>
          </button>
        );
      })}
    </div>
  );
}

