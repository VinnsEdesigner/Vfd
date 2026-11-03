/**
 * Select — a dropdown listbox built from app components, not a native <select>.
 *
 * A native <select> renders OS chrome that ignores the app's theme entirely, and
 * on some platforms it cannot be styled at all. This is a button plus a popover
 * with the same keyboard contract a real select has: Enter/Space/ArrowDown open
 * it, arrows move the active option, Enter chooses, Escape closes, and focus
 * returns to the trigger.
 *
 * The popover is positioned fixed from the trigger's rect so it escapes any
 * ancestor that clips or scrolls (the same trap the overflow menu hit).
 */

import {
  useCallback,
  useEffect,
  useId,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent,
} from 'react';
import { createPortal } from 'react-dom';
import { IconChevronDown, IconCheck } from './Icons';

export interface SelectOption {
  value: string;
  label: string;
  /** Optional second line, e.g. a state or a size. */
  detail?: string;
}

export interface SelectProps {
  /** Accessible name, e.g. "Download". */
  label: string;
  value: string;
  options: ReadonlyArray<SelectOption>;
  onChange: (value: string) => void;
  /** Render the trigger label from the selected option. */
  placeholder?: string;
  disabled?: boolean;
  id?: string;
  /** Show a type-to-filter box. Worth it past ~8 options. */
  searchable?: boolean;
  searchPlaceholder?: string;
}

const MENU_GAP = 6;

export function Select({
  label,
  value,
  options,
  onChange,
  placeholder = 'Select…',
  disabled = false,
  id,
  searchable = false,
  searchPlaceholder = 'Search…',
}: SelectProps) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const [pos, setPos] = useState<{ top: number; left: number; width: number } | null>(null);
  const [activeIndex, setActiveIndex] = useState(0);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const listRef = useRef<HTMLDivElement>(null);
  const menuId = useId();
  const generatedId = useId();
  const buttonId = id ?? generatedId;

  // Filtering is by label and detail, so a user can type either a filename or a
  // state. With no query the original order is kept, which the caller controls.
  const shown = useMemo(() => {
    if (!searchable || !query.trim()) return options;
    const q = query.trim().toLowerCase();
    return options.filter(
      (o) => o.label.toLowerCase().includes(q) || (o.detail ?? '').toLowerCase().includes(q),
    );
  }, [options, searchable, query]);

  const selectedIndex = options.findIndex((o) => o.value === value);
  const selected = selectedIndex >= 0 ? options[selectedIndex] : undefined;

  const reposition = useCallback(() => {
    const el = triggerRef.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const listH = listRef.current?.offsetHeight ?? 0;
    const spaceBelow = window.innerHeight - r.bottom;
    const openUp = listH > 0 && spaceBelow < listH + MENU_GAP && r.top > listH + MENU_GAP;
    setPos({
      top: openUp ? r.top - listH - MENU_GAP : r.bottom + MENU_GAP,
      left: r.left,
      width: r.width,
    });
  }, []);

  useLayoutEffect(() => {
    if (open) reposition();
  }, [open, reposition]);

  // Start the cursor on the current value each time the menu opens.
  useEffect(() => {
    if (!open) return;
    setQuery('');
    const i = options.findIndex((o) => o.value === value);
    setActiveIndex(i >= 0 ? i : 0);
  }, [open, options, value]);

  // Keep the cursor inside the filtered list as the query narrows it.
  useEffect(() => {
    setActiveIndex((i) => (i < shown.length ? i : Math.max(0, shown.length - 1)));
  }, [shown.length]);

  // A keyboard user must be able to see where the cursor is.
  useEffect(() => {
    if (!open) return;
    const el = listRef.current?.querySelector('.select__option.is-active');
    // jsdom (and very old browsers) have no scrollIntoView; scrolling is a
    // nicety, never a requirement.
    if (el && typeof el.scrollIntoView === 'function') {
      el.scrollIntoView({ block: 'nearest' });
    }
  }, [open, activeIndex]);

  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      const t = e.target as Node;
      if (triggerRef.current?.contains(t) || listRef.current?.contains(t)) return;
      setOpen(false);
    };
    const onMove = () => setOpen(false);
    document.addEventListener('mousedown', onDown);
    window.addEventListener('resize', onMove);
    window.addEventListener('scroll', onMove, true);
    return () => {
      document.removeEventListener('mousedown', onDown);
      window.removeEventListener('resize', onMove);
      window.removeEventListener('scroll', onMove, true);
    };
  }, [open]);

  const choose = (index: number) => {
    const opt = shown[index];
    if (opt) onChange(opt.value);
    setOpen(false);
    triggerRef.current?.focus();
  };

  const onKeyDown = (e: KeyboardEvent) => {
    if (!open) {
      if (e.key === 'ArrowDown' || e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        setOpen(true);
      }
      return;
    }
    if (e.key === 'Escape') {
      e.preventDefault();
      setOpen(false);
      triggerRef.current?.focus();
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      setActiveIndex((i) => Math.min(i + 1, shown.length - 1));
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      setActiveIndex((i) => Math.max(i - 1, 0));
    } else if (e.key === 'Home') {
      e.preventDefault();
      setActiveIndex(0);
    } else if (e.key === 'End') {
      e.preventDefault();
      setActiveIndex(shown.length - 1);
    } else if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      choose(activeIndex);
    }
  };

  const list =
    open && pos
      ? createPortal(
          <div
            id={menuId}
            ref={listRef}
            className="select__menu"
            role="listbox"
            aria-label={label}
            style={{ position: 'fixed', top: pos.top, left: pos.left, width: pos.width }}
          >
            {searchable && (
              <input
                className="select__search"
                value={query}
                placeholder={searchPlaceholder}
                aria-label={searchPlaceholder}
                autoComplete="off"
                spellCheck={false}
                onChange={(e) => setQuery(e.target.value)}
                onKeyDown={onKeyDown}
              />
            )}
            {shown.length === 0 && (
              <div className="select__empty">No matches</div>
            )}
            {shown.map((o, i) => (
              <button
                key={o.value}
                type="button"
                role="option"
                aria-selected={o.value === value}
                className={i === activeIndex ? 'select__option is-active' : 'select__option'}
                onMouseEnter={() => setActiveIndex(i)}
                onClick={() => choose(i)}
              >
                <span className="select__tick">
                  {o.value === value && <IconCheck />}
                </span>
                <span className="select__text">
                  <span className="select__label">{o.label}</span>
                  {o.detail !== undefined && <span className="select__detail">{o.detail}</span>}
                </span>
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
        id={buttonId}
        type="button"
        className="select__trigger"
        role="combobox"
        aria-label={label}
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-controls={open ? menuId : undefined}
        disabled={disabled}
        onClick={() => setOpen((v) => !v)}
        onKeyDown={onKeyDown}
      >
        <span className={selected ? 'select__value' : 'select__value is-placeholder'}>
          {selected ? selected.label : placeholder}
        </span>
        <IconChevronDown />
      </button>
      {list}
    </>
  );
}
