/**
 * FileViewer tests — the convert path for a container the browser cannot play.
 *
 * The point of the conversion is that a non-playable MKV becomes playable in
 * place, so these pin the three moments: the offer before it is asked for, the
 * status while it runs, and the MP4 playing once it is done.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { FileViewer } from './FileViewer';
import type { DownloadView } from '../../hooks/view-models';
import type { ConversionState } from '../../domain/media';

function view(patch: Partial<DownloadView> = {}): DownloadView {
  return {
    id: 'j1',
    url: 'http://x/movie.mkv',
    name: 'movie.mkv',
    source: 'engine',
    localUrl: null,
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
    conversion: { state: 'none', error: null },
    artifacts: [],
    error: null,
    createdAtLabel: '—',
    createdAt: 1,
    kindLabel: 'MKV',
    filename: 'movie.mkv',
    resolutionLabel: null,
    extension: 'mkv',
    isActive: false,
    isResumable: false,
    isSettled: true,
    isOpenable: true,
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
  };
}

function renderViewer(props: Partial<React.ComponentProps<typeof FileViewer>> = {}) {
  const onConvert = vi.fn();
  const utils = render(
    <FileViewer
      file={view()}
      src="http://x/movie.mkv/content"
      downloadUrl="http://x/movie.mkv/content"
      location="Vyzor library"
      onClose={() => undefined}
      onOpenWith={() => undefined}
      onShare={() => undefined}
      onDelete={() => undefined}
      onConvert={onConvert}
      {...props}
    />,
  );
  return { ...utils, onConvert };
}

describe('FileViewer conversion', () => {
  it('offers to convert an MKV that cannot be played here', () => {
    renderViewer();
    expect(screen.getByRole('button', { name: 'Convert to MP4' })).toBeTruthy();
  });

  it('calls back when the conversion is asked for', () => {
    const { onConvert } = renderViewer();
    fireEvent.click(screen.getByRole('button', { name: 'Convert to MP4' }));
    expect(onConvert).toHaveBeenCalledTimes(1);
  });

  it('shows the conversion running and disables the offer', () => {
    renderViewer({ converting: true });
    const button = screen.getByRole('button', { name: 'Converting…' }) as HTMLButtonElement;
    expect(button.disabled).toBe(true);
    expect(screen.getByText(/Converting to MP4/)).toBeTruthy();
  });

  it('reports a failed conversion rather than pretending it worked', () => {
    renderViewer({
      file: view({ conversion: { state: 'failed' as ConversionState, error: 'ffmpeg said no' } }),
      conversionError: 'ffmpeg said no',
    });
    expect(screen.getAllByText(/ffmpeg said no/).length).toBeGreaterThan(0);
  });

  it('plays the converted MP4 instead of the original container', () => {
    const { container } = renderViewer({
      file: view({ conversion: { state: 'done' as ConversionState, error: null } }),
      convertedUrl: 'http://x/j1/converted',
    });
    const video = container.querySelector('video') as HTMLVideoElement;
    expect(video).toBeTruthy();
    expect(video.getAttribute('src')).toBe('http://x/j1/converted');
    // The unplayable fallback is gone: the conversion replaced it.
    expect(screen.queryByText(/can’t be previewed here/)).toBeNull();
  });

  it('does not offer a conversion for a device copy', () => {
    renderViewer({ onConvert: undefined, file: view({ source: 'device' }) });
    expect(screen.queryByRole('button', { name: 'Convert to MP4' })).toBeNull();
  });
});
