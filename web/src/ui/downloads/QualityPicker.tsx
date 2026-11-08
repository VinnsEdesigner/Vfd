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
 *
 * `onDismiss` puts a close control on the picker. The card that holds a picker is
 * a *proposal*, not a decision — "here is what this source offers" — and the user
 * needs a way to say "not interested" without picking something or navigating
 * away. A dismiss belongs on the picker rather than each host card so every place
 * the picker appears gets the same affordance and the same label.
 */

import { Button } from '../primitives/Button';
import { IconX } from '../primitives/Icons';
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
  /**
   * Hide the whole card this picker is in. When given, a close button is drawn
   * above the options. Absent means the picker is not dismissable — the right
   * default for the ingest form, where it is part of the decision being made.
   */
  onDismiss?: () => void;
  /** What the dismiss button hides, for its accessible name. */
  dismissLabel?: string;
}

export function QualityPicker({
  options,
  selectedHeight,
  onSelect,
  variant = 'quality',
  emptyLabel,
  onDismiss,
  dismissLabel,
}: QualityPickerProps) {
  const isFile = variant === 'file';

  /*
   * A dismiss button is shown even when there is nothing to choose, because the
   * card it hides is still on screen — an empty picker with no way out is the
   * trap this exists to prevent.
   */
  const header = onDismiss ? (
    <div className="quality__head">
      <span className="quality__headlabel">
        {dismissLabel ?? (isFile ? 'Files' : 'Quality')}
      </span>
      <Button
        size="sm"
        variant="ghost"
        icon
        aria-label={dismissLabel ? `Dismiss ${dismissLabel}` : 'Dismiss'}
        title="Dismiss"
        onClick={onDismiss}
      >
        <IconX />
      </Button>
    </div>
  ) : null;

  if (options.length === 0) {
    return (
      <>
        {header}
        <div className="quality__empty">
          {emptyLabel ??
            'No selectable renditions — this source is a single file, so it downloads as-is.'}
        </div>
      </>
    );
  }

  return (
    <>
      {header}
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
    </>
  );
}


