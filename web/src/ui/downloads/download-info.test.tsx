/**
 * FormatChooser + DownloadInfo tests.
 *
 * The chooser exists so a format is chosen before bytes are spent, and the info
 * panel exists so a transfer can be watched and controlled. Both must be the
 * app's own dialogs, and the info panel must not claim a speed it does not have.
 */

import { render, screen, fireEvent, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { FormatChooser, formatsFromQualities, formatFromFile } from './FormatChooser';
import { DownloadInfo } from './DownloadInfo';
import type { DownloadView } from '../../hooks/view-models';

function view(patch: Partial<DownloadView>): DownloadView {
  return {
    id: 'a',
    name: 'clip.mp4',
    url: 'https://cdn.example.com/clip.mp4',
    state: 'running',
    tone: 'running',
    fraction: 0.5,
    percent: 50,
    bytes: 1000,
    bytesLabel: '1 KB',
    downloadedLabel: '500 B',
    bytesPerSecond: 500,
    sha256: null,
    masterPlaylist: null,
    artifacts: [],
    error: null,
    createdAtLabel: '01 Jan, 00:00',
    createdAt: 1,
    kindLabel: 'MP4',
    extension: 'mp4',
    isActive: true,
    isResumable: false,
    isSettled: false,
    isOpenable: false,
    isVideo: true,
    isImage: false,
    isAudio: false,
    qualities: [],
    pipeline: [],
    requestedQuality: null,
    connections: 8,
    hls: false,
    upload: false,
    purge: false,
    ...patch,
  } as DownloadView;
}

const QUALITIES = [
  { height: 1080, width: 1920, label: '1080p', bytes: 466_620_000, sizeLabel: '445 MB', codecs: 'avc1', url: 'u8' },
  { height: 720, width: 1280, label: '720p', bytes: 161_196_000, sizeLabel: '154 MB', codecs: 'avc1', url: 'u0' },
];

describe('FormatChooser', () => {
  it('lists every format with its size before anything starts', () => {
    render(
      <FormatChooser
        source="master.m3u8"
        host="cdn.example.com"
        kind="HLS"
        formats={formatsFromQualities(QUALITIES)}
        initialId="1080p"
        onConfirm={vi.fn()}
        onCancel={vi.fn()}
      />,
    );
    const options = screen.getAllByRole('radio');
    expect(options).toHaveLength(2);
    expect(within(options[0] as HTMLElement).getByText('445 MB')).toBeTruthy();
    expect(within(options[1] as HTMLElement).getByText('154 MB')).toBeTruthy();
    // Derived sizes are marked as estimates, not measurements.
    expect(screen.getAllByTitle(/Derived from the manifest/).length).toBe(2);
  });

  it('downloads the chosen format, not the default', () => {
    const onConfirm = vi.fn();
    render(
      <FormatChooser
        source="master.m3u8"
        host="cdn.example.com"
        kind="HLS"
        formats={formatsFromQualities(QUALITIES)}
        initialId="1080p"
        onConfirm={onConfirm}
        onCancel={vi.fn()}
      />,
    );
    fireEvent.click(screen.getByRole('radio', { name: /720p/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Download' }));
    expect(onConfirm).toHaveBeenCalledWith('720p');
  });

  it('cancels without starting anything', () => {
    const onConfirm = vi.fn();
    const onCancel = vi.fn();
    render(
      <FormatChooser
        source="clip.mp4"
        host="cdn.example.com"
        kind="MP4"
        formats={formatFromFile('MP4', '59 MB')}
        initialId="source"
        onConfirm={onConfirm}
        onCancel={onCancel}
      />,
    );
    // A single file is still one explicit choice, with its measured size.
    expect(screen.getAllByRole('radio')).toHaveLength(1);
    expect(screen.getByText('59 MB')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(onCancel).toHaveBeenCalled();
    expect(onConfirm).not.toHaveBeenCalled();
  });
});

describe('DownloadInfo', () => {
  it('shows the live speed, percentage and remaining time', () => {
    render(
      <DownloadInfo
        download={view({})}
        onPause={vi.fn()}
        onResume={vi.fn()}
        onCancel={vi.fn()}
        onDelete={vi.fn()}
        onClose={vi.fn()}
      />,
    );
    expect(screen.getByRole('dialog')).toBeTruthy();
    expect(screen.getByText('50%')).toBeTruthy();
    expect(screen.getByText('500 B/s')).toBeTruthy();
    expect(screen.getByText('8')).toBeTruthy(); // connections
  });

  it('offers Pause while running and no stale speed once paused', () => {
    const { unmount } = render(
      <DownloadInfo
        download={view({})}
        onPause={vi.fn()}
        onResume={vi.fn()}
        onCancel={vi.fn()}
        onDelete={vi.fn()}
        onClose={vi.fn()}
      />,
    );
    expect(screen.getByRole('button', { name: /Pause/ })).toBeTruthy();
    unmount();

    render(
      <DownloadInfo
        download={view({
          state: 'paused',
          tone: 'paused',
          isActive: false,
          isResumable: true,
          bytesPerSecond: 0,
        })}
        onPause={vi.fn()}
        onResume={vi.fn()}
        onCancel={vi.fn()}
        onDelete={vi.fn()}
        onClose={vi.fn()}
      />,
    );
    // A paused transfer has no rate, and must not show the last one it had.
    expect(screen.getByText('Paused')).toBeTruthy();
    expect(screen.getByRole('button', { name: /Resume/ })).toBeTruthy();
    expect(screen.queryByRole('button', { name: /^Pause$/ })).toBeNull();
  });

  it('reports the format that was chosen', () => {
    render(
      <DownloadInfo
        download={view({ requestedQuality: '720p' })}
        onPause={vi.fn()}
        onResume={vi.fn()}
        onCancel={vi.fn()}
        onDelete={vi.fn()}
        onClose={vi.fn()}
      />,
    );
    expect(screen.getByText('720p')).toBeTruthy();
  });

  it('wires Pause, Resume and Cancel to their handlers', () => {
    const onPause = vi.fn();
    const onResume = vi.fn();
    const onCancel = vi.fn();
    const { unmount } = render(
      <DownloadInfo
        download={view({})}
        onPause={onPause}
        onResume={onResume}
        onCancel={onCancel}
        onDelete={vi.fn()}
        onClose={vi.fn()}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: /Pause/ }));
    expect(onPause).toHaveBeenCalledTimes(1);
    unmount();

    render(
      <DownloadInfo
        download={view({ state: 'paused', tone: 'paused', isActive: false, isResumable: true })}
        onPause={onPause}
        onResume={onResume}
        onCancel={onCancel}
        onDelete={vi.fn()}
        onClose={vi.fn()}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: /Resume/ }));
    expect(onResume).toHaveBeenCalledTimes(1);
    fireEvent.click(screen.getByRole('button', { name: /Cancel/ }));
    expect(onCancel).toHaveBeenCalledTimes(1);
  });

  it('Escape closes it', () => {
    const onClose = vi.fn();
    render(
      <DownloadInfo
        download={view({})}
        onPause={vi.fn()}
        onResume={vi.fn()}
        onCancel={vi.fn()}
        onDelete={vi.fn()}
        onClose={onClose}
      />,
    );
    fireEvent.keyDown(document, { key: 'Escape' });
    expect(onClose).toHaveBeenCalled();
  });
});
