/**
 * The quality picker's dismiss control.
 *
 * A card holding a picker is a *proposal* — "here is what this source offers" —
 * and the user needs a way to decline it without choosing a rendition or
 * navigating away. The dismiss lives on the picker so every host gets the same
 * affordance, and these tests pin the two things that matter: it is only drawn
 * when the caller asks for it, and pressing it calls back exactly once.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { QualityPicker } from './QualityPicker';
import type { QualityOption } from '../../hooks/view-models';

const OPTIONS: QualityOption[] = [
  { url: 'http://x/a', height: 1080, width: 1920, label: '1080p', codecs: '', bytes: 100, sizeLabel: '100 B' },
  { url: 'http://x/a', height: 720, width: 1280, label: '720p', codecs: '', bytes: 50, sizeLabel: '50 B' },
];

describe('QualityPicker dismiss', () => {
  it('has no dismiss control when the caller does not ask for one', () => {
    // The ingest form is the case: the picker is part of the decision being
    // made, so there is nothing to dismiss.
    render(<QualityPicker options={OPTIONS} selectedHeight={1080} onSelect={() => {}} />);
    expect(screen.queryByRole('button', { name: /dismiss/i })).toBeNull();
  });

  it("calls onDismiss when the close control is pressed", () => {
    const onDismiss = vi.fn();
    render(
      <QualityPicker
        options={OPTIONS}
        selectedHeight={1080}
        onSelect={() => {}}
        onDismiss={onDismiss}
      />,
    );
    const close = screen.getByRole('button', { name: /dismiss/i });
    fireEvent.click(close);
    expect(onDismiss).toHaveBeenCalledTimes(1);
  });

  it('still offers a dismiss when there is nothing to choose', () => {
    // An empty picker with no way out is the trap this exists to prevent, so the
    // close control is drawn even with zero options.
    render(
      <QualityPicker
        options={[]}
        selectedHeight={null}
        onSelect={() => {}}
        onDismiss={() => {}}
      />,
    );
    expect(screen.getByRole('button', { name: /dismiss/i })).toBeTruthy();
  });

  it('names the dismiss after what it hides, when told', () => {
    render(
      <QualityPicker
        options={OPTIONS}
        selectedHeight={1080}
        onSelect={() => {}}
        onDismiss={() => {}}
        dismissLabel="Files"
      />,
    );
    expect(screen.getByRole('button', { name: /dismiss files/i })).toBeTruthy();
  });
});
