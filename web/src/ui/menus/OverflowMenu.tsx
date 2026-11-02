/**
 * OverflowMenu — a "•••" button that drops a small action list.
 *
 * The list renders in a **portal** attached to `document.body`, positioned
 * from the trigger's own rect. That is deliberate: a table cell sets
 * `overflow: hidden` so long text can ellipsise, and the card body scrolls
 * horizontally — either would clip an in-flow dropdown, making it look like
 * the menu simply never opened.
 *
 * Closes on outside click, Escape, or after an action runs.
 */

import { useCallback, useEffect, useId, useLayoutEffect, useRef, useState, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import { IconMore } from '../primitives/Icons';

export interface MenuAction {
  id: string;
  label: string;
  icon?: ReactNode;
  onSelect: () => void;
  /** Renders in the destructive colour; the caller confirms. */
  danger?: boolean;
  disabled?: boolean;
}

export interface OverflowMenuProps {
  actions: ReadonlyArray<MenuAction>;
  /** Accessible name, e.g. "Actions for clip.mp4". */
  label: string;
}

const MENU_WIDTH = 176;
const GAP = 4;

export function OverflowMenu({ actions, label }: OverflowMenuProps) {
  const [open, setOpen] = useState(false);
  const [pos, setPos] = useState<{ top: number; left: number } | null>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const listRef = useRef<HTMLDivElement>(null);
  const menuId = useId();

  /** Anchor the portal to the trigger, flipping up if it would overflow. */
  const reposition = useCallback(() => {
    const el = triggerRef.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const listH = listRef.current?.offsetHeight ?? 0;
    const spaceBelow = window.innerHeight - r.bottom;
    const openUp = listH > 0 && spaceBelow < listH + GAP && r.top > listH + GAP;
    const left = Math.min(Math.max(8, r.right - MENU_WIDTH), window.innerWidth - MENU_WIDTH - 8);
    setPos({ top: openUp ? r.top - listH - GAP : r.bottom + GAP, left });
  }, []);

  useLayoutEffect(() => {
    if (open) reposition();
  }, [open, reposition]);

  useEffect(() => {
    if (!open) return;
    const onDocDown = (e: MouseEvent) => {
      const target = e.target as Node;
      if (triggerRef.current?.contains(target) || listRef.current?.contains(target)) return;
      setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false);
    };
    // Any scroll or resize invalidates the measured position; closing is
    // cheaper and less surprising than chasing the trigger around.
    const onMove = () => setOpen(false);

    document.addEventListener('mousedown', onDocDown);
    document.addEventListener('keydown', onKey);
    window.addEventListener('resize', onMove);
    window.addEventListener('scroll', onMove, true);
    return () => {
      document.removeEventListener('mousedown', onDocDown);
      document.removeEventListener('keydown', onKey);
      window.removeEventListener('resize', onMove);
      window.removeEventListener('scroll', onMove, true);
    };
  }, [open]);

  if (actions.length === 0) return null;

  const list =
    open && pos
      ? createPortal(
          <div
            id={menuId}
            ref={listRef}
            className="menu__list"
            role="menu"
            style={{ position: 'fixed', top: pos.top, left: pos.left, width: MENU_WIDTH }}
          >
            {actions.map((a) => (
              <button
                key={a.id}
                type="button"
                role="menuitem"
                className={a.danger ? 'menu__item menu__item--danger' : 'menu__item'}
                disabled={a.disabled}
                onClick={(e) => {
                  e.stopPropagation();
                  setOpen(false);
                  a.onSelect();
                }}
              >
                {a.icon !== undefined && <span className="menu__icon">{a.icon}</span>}
                {a.label}
              </button>
            ))}
          </div>,
          document.body,
        )
      : null;

  return (
    <>
      <button
        ref={triggerRef}
        type="button"
        className="btn btn--sm btn--ghost btn--icon menu__trigger"
        aria-label={label}
        aria-haspopup="menu"
        aria-expanded={open}
        aria-controls={open ? menuId : undefined}
        onClick={(e) => {
          e.stopPropagation();
          setOpen((v) => !v);
        }}
      >
        <IconMore />
      </button>
      {list}
    </>
  );
}
