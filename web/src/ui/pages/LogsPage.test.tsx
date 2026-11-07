/**
 * LogsPage tests.
 *
 * The log view exists because the server logs to stderr, which a console cannot
 * see — so a failed crawl or download had no visible reason. These pin the
 * things that make it useful: the level filter (a segmented control with counts)
 * narrows to what matters, the counts say "1 error" without switching, text
 * search works, and the empty states are distinct.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { LogsPage } from './LogsPage';
import type { LogLine } from '../../hooks';

const lines: LogLine[] = [
  { seq: 1, timestamp: 1_700_000_000_000, level: 'info', text: 'crawl: reading the index' },
  { seq: 2, timestamp: 1_700_000_001_000, level: 'error', text: 'crawl: robots.txt disallows https://x/' },
  { seq: 3, timestamp: 1_700_000_002_000, level: 'warn', text: 'crawl: /a is a near-duplicate; links only' },
  { seq: 4, timestamp: 1_700_000_003_000, level: 'debug', text: 'crawl: 12 links seen' },
];

function setup(patch: Partial<Parameters<typeof LogsPage>[0]> = {}) {
  const onClear = vi.fn();
  render(<LogsPage lines={lines} enabled loaded error={null} onClear={onClear} {...patch} />);
  return { onClear };
}

describe('LogsPage', () => {
  it('shows every line by default', () => {
    setup();
    expect(screen.getByText(/reading the index/)).toBeDefined();
    expect(screen.getByText(/robots.txt disallows/)).toBeDefined();
    expect(screen.getByText(/near-duplicate/)).toBeDefined();
    expect(screen.getByText(/12 links seen/)).toBeDefined();
  });

  it('narrows to a level, and shows the count on the filter', () => {
    setup();
    // The segmented control carries a count per level, so an error is visible
    // without switching to the filter at all.
    const errorBtn = screen.getByRole('button', { name: /Error\s*1/ });
    expect(errorBtn).toBeDefined();
    fireEvent.click(errorBtn);
    expect(screen.getByText(/robots.txt disallows/)).toBeDefined();
    expect(screen.queryByText(/reading the index/)).toBeNull();
    expect(screen.queryByText(/near-duplicate/)).toBeNull();
  });

  it('filters by text', () => {
    setup();
    fireEvent.change(screen.getByLabelText('Search'), { target: { value: 'links' } });
    expect(screen.getByText(/12 links seen/)).toBeDefined();
    expect(screen.queryByText(/robots.txt disallows/)).toBeNull();
  });

  it('clears the view', () => {
    const { onClear } = setup();
    fireEvent.click(screen.getByRole('button', { name: 'Clear' }));
    expect(onClear).toHaveBeenCalled();
  });

  it('says so when the server is not capturing', () => {
    setup({ enabled: false, lines: [] });
    expect(screen.getByText(/not capturing log lines/)).toBeDefined();
  });

  it('says "no lines yet" when the ring is empty', () => {
    setup({ lines: [] });
    expect(screen.getByText(/No lines yet/)).toBeDefined();
  });

  it('says "no lines match" when a filter hides everything', () => {
    setup();
    fireEvent.change(screen.getByLabelText('Search'), { target: { value: 'zzz' } });
    expect(screen.getByText(/No lines match/)).toBeDefined();
  });
});
