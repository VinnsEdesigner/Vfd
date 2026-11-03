/**
 * useLongPress — press-and-hold, the way a touch file manager starts a
 * selection.
 *
 * Returns handlers to spread on an item. A hold past `delay` fires `onLongPress`
 * and swallows the click that follows the release, so a hold never also acts as
 * a tap. Moving more than `moveTolerance` cancels, so a scroll or drag over an
 * item is not mistaken for a hold.
 *
 * The native context menu is suppressed only for touch/pen pointers: on a
 * desktop the right-click menu is the user's, not ours.
 */

import { useCallback, useEffect, useRef } from 'react';
import type { PointerEvent as ReactPointerEvent, MouseEvent as ReactMouseEvent } from 'react';

export interface LongPressOptions {
  onLongPress: () => void;
  /** Fired for a real tap (not the release of a hold). Receives the click so
   *  the caller can honour modifier keys (shift-click for a range). */
  onTap?: (e: ReactMouseEvent) => void;
  delay?: number;
  moveTolerance?: number;
  disabled?: boolean;
}

export interface LongPressHandlers {
  onPointerDown: (e: ReactPointerEvent) => void;
  onPointerMove: (e: ReactPointerEvent) => void;
  onPointerUp: (e: ReactPointerEvent) => void;
  onPointerCancel: () => void;
  onPointerLeave: () => void;
  onContextMenu: (e: ReactMouseEvent) => void;
  onClick: (e: ReactMouseEvent) => void;
}

export function useLongPress({
  onLongPress,
  onTap,
  delay = 450,
  moveTolerance = 10,
  disabled = false,
}: LongPressOptions): LongPressHandlers {
  const timer = useRef<number | null>(null);
  const origin = useRef<{ x: number; y: number } | null>(null);
  const fired = useRef(false);
  const touchPointer = useRef(false);

  const cancelTimer = useCallback(() => {
    if (timer.current !== null) {
      window.clearTimeout(timer.current);
      timer.current = null;
    }
  }, []);

  useEffect(() => cancelTimer, [cancelTimer]);

  const onPointerDown = useCallback(
    (e: ReactPointerEvent) => {
      if (disabled) return;
      touchPointer.current = e.pointerType !== 'mouse';
      origin.current = { x: e.clientX, y: e.clientY };
      fired.current = false;
      cancelTimer();
      timer.current = window.setTimeout(() => {
        timer.current = null;
        fired.current = true;
        onLongPress();
      }, delay);
    },
    [disabled, delay, onLongPress, cancelTimer],
  );

  const onPointerMove = useCallback(
    (e: ReactPointerEvent) => {
      const start = origin.current;
      if (!start || timer.current === null) return;
      if (Math.abs(e.clientX - start.x) > moveTolerance || Math.abs(e.clientY - start.y) > moveTolerance) {
        cancelTimer();
      }
    },
    [moveTolerance, cancelTimer],
  );

  const end = useCallback(() => {
    cancelTimer();
    origin.current = null;
  }, [cancelTimer]);

  const onClick = useCallback(
    (e: ReactMouseEvent) => {
      if (fired.current) {
        // The click that follows a hold is part of the hold, not a tap.
        fired.current = false;
        e.preventDefault();
        e.stopPropagation();
        return;
      }
      onTap?.(e);
    },
    [onTap],
  );

  const onContextMenu = useCallback((e: ReactMouseEvent) => {
    if (touchPointer.current) e.preventDefault();
  }, []);

  return {
    onPointerDown,
    onPointerMove,
    onPointerUp: end,
    onPointerCancel: end,
    onPointerLeave: end,
    onContextMenu,
    onClick,
  };
}
