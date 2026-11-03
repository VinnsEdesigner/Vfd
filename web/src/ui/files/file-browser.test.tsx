/**
 * File-browser behaviour tests.
 *
 * These exercise the real components with the real hooks: the point is the
 * interaction rules (hold vs tap, filter, sort, view) rather than any one
 * component's markup. jsdom has no layout, so press-and-hold is driven through
 * the pointer events the hook listens to and the clock is advanced explicitly.
 */

import { render, screen, fireEvent, within, act } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { FileLibrary } from './FileLibrary';
import { FileToolbar } from './FileToolbar';
import { useFileBrowser } from './useFileBrowser';
import { renderHook } from '@testing-library/react';
import { shareItems, mimeForName } from './share';
import type { DownloadView } from '../../hooks/view-models';

function view(patch: Partial<DownloadView> & { id: string; name: string }): DownloadView {
  return {
    url: `http://x/${patch.name}`,
    state: 'done',
    tone: 'done',
    fraction: 1,
    percent: 100,
    bytes: 1000,
    bytesLabel: '1000 B',
    downloadedLabel: '1000 B',
    bytesPerSecond: 0,
    sha256: null,
    artifacts: [],
    error: null,
    createdAtLabel: '01 Jan, 00:00',
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
    ...patch,
  } as DownloadView;
}

const FILES: DownloadView[] = [
  view({ id: 'a', name: 'alpha.mp4', bytes: 300, createdAt: 3, kindLabel: 'MP4', extension: 'mp4' }),
  view({ id: 'b', name: 'bravo.png', bytes: 100, createdAt: 1, kindLabel: 'PNG', extension: 'png', isVideo: false, isImage: true }),
  view({ id: 'c', name: 'charlie.mp3', bytes: 200, createdAt: 2, kindLabel: 'MP3', extension: 'mp3', isVideo: false, isAudio: true }),
];

function renderLibrary(props: Partial<React.ComponentProps<typeof FileLibrary>> = {}) {
  const handlers = {
    onOpen: vi.fn(),
    onShare: vi.fn(),
    onDelete: vi.fn(),
    onMove: vi.fn(),
    onRename: vi.fn(),
  };
  render(
    <FileLibrary
      files={FILES}
      target="local"
      localPath="/tmp/library"
      cloudPath="s3://b/x"
      contentUrl={(id) => `/api/v1/downloads/${id}/content`}
      {...handlers}
      {...props}
    />,
  );
  return handlers;
}

describe('useFileBrowser', () => {
  it('filters by type and sorts by the chosen key', () => {
    const { result } = renderHook(() => useFileBrowser(FILES));
    // default: date desc -> newest first
    expect(result.current.visible.map((f) => f.id)).toEqual(['a', 'c', 'b']);

    act(() => result.current.setSort('name'));
    expect(result.current.visible.map((f) => f.id)).toEqual(['a', 'b', 'c']);

    act(() => result.current.setSort('size'));
    expect(result.current.visible.map((f) => f.id)).toEqual(['a', 'c', 'b']);

    act(() => result.current.setTypeFilter('image'));
    expect(result.current.visible.map((f) => f.id)).toEqual(['b']);
    expect(result.current.activeFilterCount).toBe(1);
  });

  it('flips direction when the active key is chosen again', () => {
    const { result } = renderHook(() => useFileBrowser(FILES));
    act(() => result.current.setSort('name'));
    expect(result.current.sortDirection).toBe('asc');
    act(() => result.current.setSort('name'));
    expect(result.current.sortDirection).toBe('desc');
  });
});

describe('FileToolbar', () => {
  it('opens a real filter menu and applies a type', () => {
    const seen: string[] = [];
    function Harness() {
      const browser = useFileBrowser(FILES);
      seen.push(browser.typeFilter);
      return <FileToolbar browser={browser} totalCount={FILES.length} />;
    }
    render(<Harness />);
    fireEvent.click(screen.getByRole('button', { name: /filter/i }));
    // The menu is real: its options are present and clickable.
    const images = screen.getByRole('menuitemradio', { name: 'Images' });
    fireEvent.click(images);
    expect(seen.at(-1)).toBe('image');
    expect(screen.getByText('1 of 3')).toBeTruthy();
  });

  it('toggles between list and grid', () => {
    render(<HarnessForToolbar />);
    const grid = screen.getByRole('button', { name: 'Grid view' });
    fireEvent.click(grid);
    expect(grid.getAttribute('aria-pressed')).toBe('true');
  });
});

function HarnessForToolbar() {
  const browser = useFileBrowser(FILES);
  return <FileToolbar browser={browser} totalCount={FILES.length} />;
}

describe('FileLibrary selection', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('starts a selection on press-and-hold and shows the action bar', () => {
    renderLibrary();
    expect(screen.queryByText(/selected/)).toBeNull();

    const card = screen.getByRole('option', { name: 'alpha.mp4' });
    fireEvent.pointerDown(card, { pointerType: 'touch', clientX: 10, clientY: 10 });
    act(() => vi.advanceTimersByTime(500));

    expect(screen.getByText('1 selected')).toBeTruthy();
    expect(screen.getByRole('button', { name: /share/i })).toBeTruthy();
    expect(screen.getByRole('button', { name: /move/i })).toBeTruthy();
    expect(screen.getByRole('button', { name: 'Delete selected' })).toBeTruthy();
  });

  it('does not open the file when the press became a hold', () => {
    const h = renderLibrary();
    const card = screen.getByRole('option', { name: 'alpha.mp4' });
    fireEvent.pointerDown(card, { pointerType: 'touch', clientX: 10, clientY: 10 });
    act(() => vi.advanceTimersByTime(500));
    // The click that follows the release belongs to the hold.
    fireEvent.click(card);
    expect(h.onOpen).not.toHaveBeenCalled();
  });

  it('toggles items once a selection exists, and opens when it does not', () => {
    const h = renderLibrary();
    const alpha = screen.getByRole('option', { name: 'alpha.mp4' });
    const bravo = screen.getByRole('option', { name: 'bravo.png' });

    // Nothing selected: a tap opens.
    fireEvent.click(alpha);
    expect(h.onOpen).toHaveBeenCalledWith('a');

    // Hold starts the selection, then a tap on another item adds it.
    fireEvent.pointerDown(alpha, { pointerType: 'touch', clientX: 1, clientY: 1 });
    act(() => vi.advanceTimersByTime(500));
    fireEvent.click(bravo);
    expect(screen.getByText('2 selected')).toBeTruthy();
    expect(h.onOpen).toHaveBeenCalledTimes(1);
  });

  it('deletes the whole selection through the action bar', () => {
    const h = renderLibrary();
    const alpha = screen.getByRole('option', { name: 'alpha.mp4' });
    fireEvent.pointerDown(alpha, { pointerType: 'touch', clientX: 1, clientY: 1 });
    act(() => vi.advanceTimersByTime(500));

    fireEvent.click(screen.getByRole('button', { name: 'Delete selected' }));
    expect(h.onDelete).toHaveBeenCalledWith(['a']);
  });

  it('shares the selected items', () => {
    const h = renderLibrary();
    const alpha = screen.getByRole('option', { name: 'alpha.mp4' });
    fireEvent.pointerDown(alpha, { pointerType: 'touch', clientX: 1, clientY: 1 });
    act(() => vi.advanceTimersByTime(500));

    fireEvent.click(screen.getByRole('button', { name: /^share$/i }));
    expect(h.onShare).toHaveBeenCalledTimes(1);
    expect((h.onShare.mock.calls[0]?.[0] as DownloadView[])[0]?.id).toBe('a');
  });

  it('moves through a dialog that pre-fills a folder', () => {
    const h = renderLibrary();
    const alpha = screen.getByRole('option', { name: 'alpha.mp4' });
    fireEvent.pointerDown(alpha, { pointerType: 'touch', clientX: 1, clientY: 1 });
    act(() => vi.advanceTimersByTime(500));

    fireEvent.click(screen.getByRole('button', { name: /move/i }));
    const dialog = screen.getByRole('dialog');
    const folder = within(dialog).getByLabelText('Folder') as HTMLInputElement;
    expect(folder.value).toBe('Videos');

    fireEvent.change(folder, { target: { value: 'Archive' } });
    fireEvent.click(within(dialog).getByRole('button', { name: 'Move' }));
    expect(h.onMove).toHaveBeenCalledWith(['a'], 'Archive');
  });
});

describe('preference persistence', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it('keeps view, sort and filters across an unmount (tab switch)', () => {
    const first = render(
      <FileLibrary
        files={FILES}
        target="local"
        localPath="/tmp/l"
        cloudPath="s3://b"
        contentUrl={(id) => `/x/${id}`}
        onOpen={vi.fn()}
        onShare={vi.fn()}
        onDelete={vi.fn()}
        onMove={vi.fn()}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: 'Grid view' }));
    fireEvent.click(screen.getByRole('button', { name: /^Sort/ }));
    fireEvent.click(screen.getByRole('menuitemradio', { name: 'Name' }));
    fireEvent.click(screen.getByRole('button', { name: /^Filter/ }));
    fireEvent.click(screen.getByRole('menuitemradio', { name: 'Images' }));
    expect(screen.getByRole('listbox').className).toBe('fgrid');
    first.unmount();

    // Remounting is what a tab switch does: the component is gone, the
    // preference is not.
    render(
      <FileLibrary
        files={FILES}
        target="local"
        localPath="/tmp/l"
        cloudPath="s3://b"
        contentUrl={(id) => `/x/${id}`}
        onOpen={vi.fn()}
        onShare={vi.fn()}
        onDelete={vi.fn()}
        onMove={vi.fn()}
      />,
    );
    expect(screen.getByRole('listbox').className).toBe('fgrid');
    expect(screen.getByRole('button', { name: /^Filter/ }).textContent).toContain('1');
    expect(screen.getByRole('button', { name: /^Sort/ }).textContent).toContain('Name');
  });

  it('persists preferences but never the selection', () => {
    renderLibrary();
    const card = screen.getByRole('option', { name: 'alpha.mp4' });
    fireEvent.pointerDown(card, { pointerType: 'touch', clientX: 1, clientY: 1 });
    act(() => vi.advanceTimersByTime(500));
    expect(screen.getByText('1 selected')).toBeTruthy();

    // The selection is about specific items and would be stale after a reload,
    // so it must not be written to storage with the preferences.
    const raw = localStorage.getItem('vyzorix.files.preferences') ?? '';
    expect(raw).not.toContain('alpha.mp4');
    expect(useFileBrowserStore.getState()).not.toHaveProperty('ids');
  });
});

describe('shareItems', () => {
  it('uses the file tier when the platform accepts files', async () => {
    const share = vi.fn().mockResolvedValue(undefined);
    vi.stubGlobal('navigator', { share, canShare: () => true, clipboard: undefined });
    const fetchImpl = vi.fn().mockResolvedValue(
      new Response(new Uint8Array([1, 2, 3]), { status: 200 }),
    );

    const outcome = await shareItems(
      [{ id: 'a', name: 'clip.mp4', url: 'http://x/a', mime: 'video/mp4' }],
      { fetchImpl: fetchImpl as unknown as typeof fetch },
    );

    expect(outcome.method).toBe('files');
    const data = share.mock.calls[0]?.[0] as { files: File[] };
    expect(data.files[0]?.name).toBe('clip.mp4');
  });

  it('falls back to a URL share when files are unsupported', async () => {
    const share = vi.fn().mockResolvedValue(undefined);
    vi.stubGlobal('navigator', { share, canShare: () => false });
    const outcome = await shareItems([
      { id: 'a', name: 'clip.mp4', url: 'http://x/a', mime: 'video/mp4' },
    ]);
    expect(outcome.method).toBe('url');
    expect(share.mock.calls[0]?.[0]).toMatchObject({ url: 'http://x/a' });
  });

  it('copies links when there is no share API at all', async () => {
    const writeText = vi.fn().mockResolvedValue(undefined);
    vi.stubGlobal('navigator', { clipboard: { writeText } });
    const outcome = await shareItems([
      { id: 'a', name: 'clip.mp4', url: 'http://x/a', mime: 'video/mp4' },
    ]);
    expect(outcome.method).toBe('clipboard');
    expect(writeText).toHaveBeenCalledWith('http://x/a');
  });

  it('treats a cancelled share sheet as not-an-error', async () => {
    const share = vi.fn().mockRejectedValue(new DOMException('abort', 'AbortError'));
    vi.stubGlobal('navigator', { share, canShare: () => false });
    const outcome = await shareItems([
      { id: 'a', name: 'clip.mp4', url: 'http://x/a', mime: 'video/mp4' },
    ]);
    expect(outcome.method).toBe('cancelled');
  });
});

describe('mimeForName', () => {
  it('maps known extensions and defaults sensibly', () => {
    expect(mimeForName('a.mp4')).toBe('video/mp4');
    expect(mimeForName('a.PNG')).toBe('image/png');
    expect(mimeForName('a.flac')).toBe('audio/flac');
    expect(mimeForName('noext')).toBe('application/octet-stream');
  });
});
