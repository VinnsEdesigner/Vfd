/**
 * QualityPicker — choose a rendition when the source is a manifest.
 *
 * Only rendered when the source actually advertises variants. Sizes come from
 * the manifest's own BANDWIDTH (times the duration we were told); when that is
 * not derivable the row says "size unknown" rather than showing a guess.
 */

import type { QualityOption } from '../../hooks/view-models';

export interface QualityPickerProps {
  options: ReadonlyArray<QualityOption>;
  selectedHeight: number | null;
  onSelect: (height: number) => void;
}

export function QualityPicker({ options, selectedHeight, onSelect }: QualityPickerProps) {
  if (options.length === 0) {
    return (
      <div className="quality__empty">
        No selectable renditions — this source is a single file, so it downloads as-is.
      </div>
    );
  }

  return (
    <div className="quality" role="radiogroup" aria-label="Download quality">
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
            <span className="quality__badge">{q.label}</span>
            <span className="quality__dims">
              {q.width > 0 && q.height > 0 ? `${q.width}×${q.height}` : '—'}
            </span>
            {q.codecs !== '' && <span className="quality__codecs">{q.codecs}</span>}
            <span className={q.bytes >= 0 ? 'quality__size' : 'quality__size quality__size--unknown'}>
              {q.sizeLabel}
            </span>
          </button>
        );
      })}
    </div>
  );
}
