/**
 * Turning DOM input into the engine's commands.
 *
 * Kept pure and separate from the component so it can be tested without a
 * browser: the interesting part is the arithmetic (which coordinates, which
 * modifier bits) and the key mapping, not the React wiring around it.
 *
 * The wire shapes here match `ws_dispatch` in the engine's `mi_server.c`; the
 * engine's names are the contract, so they are spelled the same way.
 */

/**
 * The CDP modifier bitfield.
 *
 * These are CDP's numbers, not the DOM's: Alt is 1, Ctrl is 2, Meta is 4, Shift
 * is 8. Getting them from the wrong source would send the wrong modifier and is
 * the kind of bug that only shows up when someone holds Ctrl and clicks.
 */
export function modifiersFromEvent(e: {
  altKey?: boolean;
  ctrlKey?: boolean;
  metaKey?: boolean;
  shiftKey?: boolean;
}): number {
  return (
    (e.altKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.metaKey ? 4 : 0) | (e.shiftKey ? 8 : 0)
  );
}

/** The buttons bitfield for a mouse event, from the DOM's `buttons`. */
export function mouseButtons(e: { buttons?: number }): number {
  return typeof e.buttons === 'number' ? e.buttons : 0;
}

export interface LiveMouseMessage {
  type: 'mouse';
  event: 'mousePressed' | 'mouseReleased' | 'mouseMoved';
  x: number;
  y: number;
  button: 'left' | 'middle' | 'right' | 'none';
  click_count: number;
  buttons: number;
  modifiers: number;
}

export function mouseMessage(
  event: LiveMouseMessage['event'],
  x: number,
  y: number,
  button: LiveMouseMessage['button'],
  clickCount: number,
  buttons: number,
  modifiers: number,
): LiveMouseMessage {
  return { type: 'mouse', event, x, y, button, click_count: clickCount, buttons, modifiers };
}

export interface LiveWheelMessage {
  type: 'wheel';
  x: number;
  y: number;
  dx: number;
  dy: number;
  modifiers: number;
}

export function wheelMessage(
  x: number,
  y: number,
  dx: number,
  dy: number,
  modifiers: number,
): LiveWheelMessage {
  return { type: 'wheel', x, y, dx, dy, modifiers };
}

export interface LiveKeyMessage {
  type: 'key';
  event: 'keyDown' | 'keyUp' | 'rawKeyDown' | 'char';
  key: string;
  code: string;
  vk: number;
  text?: string;
  modifiers: number;
}

/**
 * A key event.
 *
 * `vk` is the DOM's legacy `keyCode`, which browsers still populate and which
 * corresponds to CDP's `windowsVirtualKeyCode`. `text` is set only for a
 * printable key pressed without Ctrl/Meta, because a page's text input comes from
 * `text` while its shortcuts come from the key code — sending both for Ctrl+C
 * would type a "c".
 */
export function keyMessage(
  event: LiveKeyMessage['event'],
  e: {
    key: string;
    code: string;
    keyCode: number;
    altKey?: boolean;
    ctrlKey?: boolean;
    metaKey?: boolean;
    shiftKey?: boolean;
  },
): LiveKeyMessage {
  const modifiers = modifiersFromEvent(e);
  const printable = e.key.length === 1 && !e.ctrlKey && !e.metaKey;
  const msg: LiveKeyMessage = {
    type: 'key',
    event,
    key: e.key,
    code: e.code,
    vk: e.keyCode,
    modifiers,
  };
  if (event === 'keyDown' && printable) msg.text = e.key;
  return msg;
}

export interface LiveTextMessage {
  type: 'text';
  text: string;
}

/** Committed text (paste, IME), which is not a key sequence. */
export function textMessage(text: string): LiveTextMessage {
  return { type: 'text', text };
}

export interface LiveTouchMessage {
  type: 'touch';
  event: 'touchStart' | 'touchMove' | 'touchEnd' | 'touchCancel';
  /** Flat x,y pairs, which is the shape the engine's parser reads. */
  points: number[];
  modifiers: number;
}

export function touchMessage(
  event: LiveTouchMessage['event'],
  points: ReadonlyArray<{ x: number; y: number }>,
  modifiers: number,
): LiveTouchMessage {
  const flat: number[] = [];
  for (const p of points) {
    flat.push(p.x, p.y);
  }
  return { type: 'touch', event, points: flat, modifiers };
}

export interface LiveNavigateMessage {
  type: 'navigate';
  url: string;
}

export function navigateMessage(url: string): LiveNavigateMessage {
  return { type: 'navigate', url };
}

export interface LiveResizeMessage {
  type: 'resize';
  width: number;
  height: number;
}

export function resizeMessage(width: number, height: number): LiveResizeMessage {
  return { type: 'resize', width: Math.round(width), height: Math.round(height) };
}

/**
 * Map a point on a displayed canvas onto the frame's own pixel grid.
 *
 * The canvas is usually shown at a different size from the frame it holds, so
 * a click at the visual position must be scaled to the frame's coordinates or
 * every click lands in the wrong place on a high-DPI or a shrunken view.
 */
export function toFrameCoords(
  clientX: number,
  clientY: number,
  rect: { left: number; top: number; width: number; height: number },
  frameWidth: number,
  frameHeight: number,
): { x: number; y: number } {
  if (rect.width <= 0 || rect.height <= 0) return { x: 0, y: 0 };
  const x = ((clientX - rect.left) / rect.width) * frameWidth;
  const y = ((clientY - rect.top) / rect.height) * frameHeight;
  return { x: Math.max(0, Math.min(frameWidth, x)), y: Math.max(0, Math.min(frameHeight, y)) };
}
