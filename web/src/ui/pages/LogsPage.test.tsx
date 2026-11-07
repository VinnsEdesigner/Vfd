/**
 * LogsPage tests.
 *
 * The log view exists because the server logs to stderr, which a console cannot
 * see — so a failed crawl or download had no visible reason. These pin the two
 * things that make it useful: a level filter narrows to what matters, and an
 * error is visually distinguished from the chatter.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { LogsPage } from './LogsPage';
import type { LogLine } from '../../hooks';

const lines: LogLine[] = [
  { seq: 1, timestamp: 1_700_000_000_000, level: 'info', text: 'crawl: reading the index' },
  { seq: 2, timestamp: 1_700_000_001_000, level: 'error', text: 'crawl: robots.txt disallows https://x/' },
  { seq: 3, timestamp: 1_700_000_002_000, level: 'debug', text: 'crawl: 12 links seen' },
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
    expect(screen.getByText(/12 links seen/)).toBeDefined();
  });

  it('narrows to errors only', () => {
    setup();
    fireEvent.click(screen.getByRole('combobox', { name: 'Level' }));
    fireEvent.click(screen.getByRole('option', { name: 'Errors only' }));
    expect(screen.getByText(/robots.txt disallows/)).toBeDefined();
    expect(screen.queryByText(/reading the index/)).toBeNull();
  });

  it('filters by text', () => {
    setup();
    fireEvent.change(screen.getByLabelText('Filter'), { target: { value: 'links' } });
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
});
