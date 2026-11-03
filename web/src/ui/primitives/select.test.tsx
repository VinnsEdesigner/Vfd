/**
 * Select and ConfirmDialog tests.
 *
 * Both exist to keep the app out of browser chrome, so the things worth
 * asserting are the behaviours a native control would have given us: a real
 * listbox with keyboard support, and a confirmation whose destructive button is
 * never the default.
 */

import { render, screen, fireEvent, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { Select } from './Select';
import { ConfirmDialog } from './ConfirmDialog';

const OPTIONS = [
  { value: 'a', label: 'alpha.mp4', detail: 'done' },
  { value: 'b', label: 'bravo.png', detail: 'running' },
  { value: 'c', label: 'charlie.mp3', detail: 'failed' },
];

describe('Select', () => {
  it('renders a combobox, not a native select', () => {
    render(<Select label="Download" value="a" options={OPTIONS} onChange={vi.fn()} />);
    expect(screen.getByRole('combobox', { name: 'Download' })).toBeTruthy();
    // The whole point: no native <select> in the tree.
    expect(document.querySelector('select')).toBeNull();
    expect(screen.getByText('alpha.mp4')).toBeTruthy();
  });

  it('opens a listbox and chooses an option with the mouse', () => {
    const onChange = vi.fn();
    render(<Select label="Download" value="a" options={OPTIONS} onChange={onChange} />);
    fireEvent.click(screen.getByRole('combobox'));
    const list = screen.getByRole('listbox', { name: 'Download' });
    expect(within(list).getAllByRole('option')).toHaveLength(3);
    fireEvent.click(within(list).getByRole('option', { name: /charlie/ }));
    expect(onChange).toHaveBeenCalledWith('c');
    // Choosing closes the list.
    expect(screen.queryByRole('listbox')).toBeNull();
  });

  it('supports the keyboard: arrows move, Enter chooses, Escape closes', () => {
    const onChange = vi.fn();
    render(<Select label="Download" value="a" options={OPTIONS} onChange={onChange} />);
    const trigger = screen.getByRole('combobox');
    fireEvent.keyDown(trigger, { key: 'ArrowDown' });
    expect(screen.getByRole('listbox')).toBeTruthy();

    fireEvent.keyDown(trigger, { key: 'ArrowDown' });
    fireEvent.keyDown(trigger, { key: 'Enter' });
    expect(onChange).toHaveBeenCalledWith('b');

    fireEvent.keyDown(trigger, { key: 'ArrowDown' });
    fireEvent.keyDown(trigger, { key: 'Escape' });
    expect(screen.queryByRole('listbox')).toBeNull();
  });

  it('shows the placeholder when nothing matches', () => {
    render(
      <Select label="X" value="missing" options={OPTIONS} onChange={vi.fn()} placeholder="Pick one" />,
    );
    expect(screen.getByText('Pick one')).toBeTruthy();
  });
});

describe('ConfirmDialog', () => {
  it('is an in-app dialog, not a native confirm', () => {
    const confirmSpy = vi.spyOn(window, 'confirm');
    render(
      <ConfirmDialog
        title="Delete this download?"
        message="The files will be removed from disk."
        details={['clip.mp4']}
        destructive
        onConfirm={vi.fn()}
        onCancel={vi.fn()}
      />,
    );
    expect(screen.getByRole('alertdialog')).toBeTruthy();
    expect(screen.getByText('clip.mp4')).toBeTruthy();
    expect(confirmSpy).not.toHaveBeenCalled();
    confirmSpy.mockRestore();
  });

  it('focuses Cancel, so Enter cannot delete by accident', () => {
    render(
      <ConfirmDialog title="Delete?" message="Gone." destructive onConfirm={vi.fn()} onCancel={vi.fn()} />,
    );
    expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Cancel' }));
  });

  it('Escape cancels and the scrim click cancels', () => {
    const onCancel = vi.fn();
    render(
      <ConfirmDialog title="Delete?" message="Gone." onConfirm={vi.fn()} onCancel={onCancel} />,
    );
    fireEvent.keyDown(document, { key: 'Escape' });
    expect(onCancel).toHaveBeenCalledTimes(1);
    // The dialog portals to document.body, so query it there.
    const scrim = document.body.querySelector('.cdialog__scrim');
    fireEvent.mouseDown(scrim as Element);
    expect(onCancel).toHaveBeenCalledTimes(2);
  });

  it('calls onConfirm only when the destructive button is pressed', () => {
    const onConfirm = vi.fn();
    render(
      <ConfirmDialog
        title="Delete?"
        message="Gone."
        confirmLabel="Delete"
        destructive
        onConfirm={onConfirm}
        onCancel={vi.fn()}
      />,
    );
    fireEvent.click(screen.getByRole('button', { name: 'Delete' }));
    expect(onConfirm).toHaveBeenCalledTimes(1);
  });
});
