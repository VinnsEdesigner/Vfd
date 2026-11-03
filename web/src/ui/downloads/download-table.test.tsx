/**
 * DownloadTable tests.
 *
 * The redesign's whole point is that a row says something useful: the name and
 * its source host, not a database id and a raw URL. These assert that, plus the
 * honest progress column (a bar only while there is progress to show).
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { DownloadTable } from './DownloadTable';
import type { DownloadView } from '../../hooks/view-models';

function view(patch: Partial<DownloadView> & { id: string }): DownloadView {
  return {
    name: 'clip.mp4',
    url: 'https://cdn.example.com/library/clip.mp4',
    state: 'done',
    tone: 'done',
    fraction: 1,
    percent: 100,
    bytes: 1024,
    bytesLabel: '1 KB',
    downloadedLabel: '1 KB',
    bytesPerSecond: 0,
    sha256: null,
    masterPlaylist: null,
    artifacts: [],
    error: null,
    createdAtLabel: 'x',
    createdAt: 1,
    kindLabel: 'MP4',
    extension: 'mp4',
    isActive: false,
    isSettled: true,
    isOpenable: true,
    isVideo: true,
    isImage: false,
    isAudio: false,
    qualities: [],
    pipeline: [],
    ...patch,
  } as DownloadView;
}

describe('DownloadTable', () => {
  it('shows the name and source host, not an id and a raw url', () => {
    render(<DownloadTable downloads={[view({ id: 'abc123' })]} />);
    expect(screen.getByText('clip.mp4')).toBeTruthy();
    expect(screen.getByText('cdn.example.com')).toBeTruthy();
    // The 32-char id must not be the row's identity.
    expect(screen.queryByText('abc123')).toBeNull();
  });

  it('shows a progress bar only while the transfer is live', () => {
    const { unmount } = render(
      <DownloadTable
        downloads={[view({ id: 'r', state: 'running', isActive: true, percent: 42, fraction: 0.42 })]}
      />,
    );
    expect(screen.getByRole('progressbar')).toBeTruthy();
    expect(screen.getByText('42%')).toBeTruthy();
    unmount();

    render(<DownloadTable downloads={[view({ id: 'd', state: 'done' })]} />);
    // A finished row is not a full bar repeated down the page.
    expect(screen.queryByRole('progressbar')).toBeNull();
    expect(screen.getByText('Done')).toBeTruthy();
  });

  it('offers Play for an openable file and a menu with Delete', () => {
    const onOpen = vi.fn();
    const onDelete = vi.fn();
    render(
      <DownloadTable
        downloads={[view({ id: 'x' })]}
        onOpen={onOpen}
        onDelete={onDelete}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: /Play/ }));
    expect(onOpen).toHaveBeenCalledWith('x');

    fireEvent.click(screen.getByRole('button', { name: /Actions for/ }));
    fireEvent.click(screen.getByRole('menuitem', { name: /Delete/ }));
    expect(onDelete).toHaveBeenCalledWith('x');
  });

  it('offers Cancel only while a transfer is in flight', () => {
    const { unmount } = render(
      <DownloadTable downloads={[view({ id: 'r', state: 'running', isActive: true })]} onCancel={vi.fn()} />,
    );
    expect(screen.getByRole('button', { name: /Cancel/ })).toBeTruthy();
    unmount();

    render(<DownloadTable downloads={[view({ id: 'd', state: 'done' })]} onCancel={vi.fn()} />);
    expect(screen.queryByRole('button', { name: /Cancel/ })).toBeNull();
  });

  it('renders an empty state rather than an empty list', () => {
    render(<DownloadTable downloads={[]} />);
    expect(screen.getByText('No downloads yet')).toBeTruthy();
  });
});
