/**
 * Turning DOM input into Vyzor's commands.
 *
 * Kept pure and separate from the component so it can be tested without a
 * browser: the interesting part is the arithmetic (which coordinates, which
 * modifier bits) and the key mapping, not the React wiring around it.
 *
 * The wire shapes here match `ws_dispatch` in Vyzor's `mi_server.c`; the
 * Vyzor's names are the contract, so they are spelled the same way.
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
  /** Flat x,y pairs, which is the shape Vyzor's parser reads. */
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
 * The rectangle the frame actually occupies inside its box.
 *
 * `object-fit: contain` letterboxes the image when its aspect ratio differs from
 * the box's, so the image does not fill its box. Mapping a click against the box
 * would then be wrong by the size of the bars — on a phone, where the frame is a
 * fixed phone shape and the box is whatever the screen gives, that is a large
 * error, and it is why a tap landed away from the field it was aimed at.
 */
export function displayedFrameRect(
  box: { left: number; top: number; width: number; height: number },
  frameWidth: number,
  frameHeight: number,
  align: 'top-left' | 'center' = 'top-left',
): { left: number; top: number; width: number; height: number } {
  if (frameWidth <= 0 || frameHeight <= 0 || box.width <= 0 || box.height <= 0) return box;
  const scale = Math.min(box.width / frameWidth, box.height / frameHeight);
  const width = frameWidth * scale;
  const height = frameHeight * scale;
  const left = align === 'center' ? box.left + (box.width - width) / 2 : box.left;
  const top = align === 'center' ? box.top + (box.height - height) / 2 : box.top;
  return { left, top, width, height };
}

/**
 * Map a point on a displayed frame onto the frame's own pixel grid.
 *
 * The frame is drawn at a different size from the one it holds, so a click must
 * be scaled to the frame's coordinates or it lands in the wrong place. The
 * letterboxing is taken into account, because the frame does not fill its box.
 */
export function toFrameCoords(
  clientX: number,
  clientY: number,
  rect: { left: number; top: number; width: number; height: number },
  frameWidth: number,
  frameHeight: number,
): { x: number; y: number } {
  if (rect.width <= 0 || rect.height <= 0) return { x: 0, y: 0 };
  const shown = displayedFrameRect(rect, frameWidth, frameHeight);
  if (shown.width <= 0 || shown.height <= 0) return { x: 0, y: 0 };
  const x = ((clientX - shown.left) / shown.width) * frameWidth;
  const y = ((clientY - shown.top) / shown.height) * frameHeight;
  return { x: Math.max(0, Math.min(frameWidth, x)), y: Math.max(0, Math.min(frameHeight, y)) };
}
