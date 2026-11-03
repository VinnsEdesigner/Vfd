/**
 * StorageLocationPanel tests.
 *
 * The panel used to offer a "download folder" the browser could not honour, a
 * parts folder nothing read, and an "open folder on finish" switch that did
 * nothing. It now reads the directory from the engine and saving is a real
 * request. These tests pin both halves: the engine's path is what is shown, and
 * the controls that never did anything are gone.
 */

import { render, screen, fireEvent } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { StorageLocationPanel, type StorageLocationValue } from './StorageLocationPanel';

const base: StorageLocationValue = {
  target: 'local',
  bucket: 'vyzorix-media',
  keyPrefix: 'downloads/',
};

const engine = {
  workDir: '/srv/media',
  writable: true,
  writeError: null,
  mountPoint: '/srv',
  fsType: 'ext4',
};

function setup(patch: Partial<Parameters<typeof StorageLocationPanel>[0]> = {}) {
  const onSave = vi.fn();
  const onChange = vi.fn();
  render(
    <StorageLocationPanel
      value={base}
      onChange={onChange}
      engineLocation={engine}
      onSave={onSave}
      {...patch}
    />,
  );
  return { onSave, onChange };
}

describe('StorageLocationPanel', () => {
  it('shows the directory the engine actually reports', () => {
    setup();
    expect(screen.getByDisplayValue('/srv/media')).toBeTruthy();
  });

  it('names the volume the engine resolved', () => {
    setup();
    expect(screen.getByText(/\/srv \(ext4\)/)).toBeTruthy();
  });

  it('saves the edited directory to the engine', () => {
    const { onSave } = setup();
    fireEvent.change(screen.getByDisplayValue('/srv/media'), {
      target: { value: '/mnt/media' },
    });
    fireEvent.click(screen.getByRole('button', { name: /save location/i }));
    expect(onSave).toHaveBeenCalledWith('/mnt/media');
  });

  it('does not offer to save when nothing changed', () => {
    setup();
    const save = screen.getByRole('button', { name: /save location/i }) as HTMLButtonElement;
    expect(save.disabled).toBe(true);
  });

  it('surfaces a directory the engine refused', () => {
    setup({
      engineLocation: {
        ...engine,
        writable: false,
        writeError: 'directory is not writable',
      },
    });
    expect(screen.getByText(/directory is not writable/i)).toBeTruthy();
  });

  // The stubs, asserted absent so they cannot quietly come back.
  it('no longer offers a parts folder, an open-folder switch, or a write test', () => {
    setup();
    expect(screen.queryByText(/temporary parts folder/i)).toBeNull();
    expect(screen.queryByText(/open folder when a download finishes/i)).toBeNull();
    expect(screen.queryByRole('button', { name: /test write access/i })).toBeNull();
  });

  it('shows the bucket and prefix only for cloud storage', () => {
    const { onChange } = setup();
    expect(screen.queryByDisplayValue('vyzorix-media')).toBeNull();
    fireEvent.click(screen.getByRole('radio', { name: /cloud storage/i }));
    expect(onChange).toHaveBeenCalledWith({ ...base, target: 'cloud' });
  });
});
