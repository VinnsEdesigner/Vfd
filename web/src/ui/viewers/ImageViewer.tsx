/**
 * ImageViewer — full-page image with zoom, pan, rotate and fit.
 *
 * Two input models, because a phone and a desktop do not gesture the same way:
 *
 *   pointer  one pointer drags, the wheel zooms at the cursor, buttons and keys
 *            are shortcuts. Zoom is anchored at the cursor so wheeling into a
 *            detail keeps that detail under the pointer.
 *   touch    two fingers pinch to zoom and drag together to pan, with the midpoint
 *            of the fingers as the anchor. One finger pans once the image
 *            overflows, and does nothing before that, because a drag that moves
 *            nothing feels broken.
 *
 * Pinch is implemented on raw pointer events rather than the browser's own
 * gesture handling: `touch-action: none` on the stage stops the browser claiming
 * the gesture, so both fingers arrive here and the scale is ours to control.
 *
 * Full screen uses the browser's own API on the whole viewer (bar and stage
 * together, not the picture alone), so the controls stay reachable and the
 * picture is fitted to the viewport rather than left at its old zoom.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { Button } from '../primitives/Button';
import {
  IconZoomIn,
  IconZoomOut,
  IconRotate,
  IconFit,
  IconFullscreen,
  IconExitFullscreen,
} from '../primitives/Icons';

export interface ImageViewerProps {
  src: string;
  alt: string;
  onDimensions?: (dims: { width: number; height: number }) => void;
}

const MIN_SCALE = 0.1;
const MAX_SCALE = 8;
/** Pointer distance must change by this much before it counts as a pinch. */
const PINCH_THRESHOLD_PX = 4;

interface Point {
  x: number;
  y: number;
}

export function ImageViewer({ src, alt, onDimensions }: ImageViewerProps) {
  const [scale, setScale] = useState(1);
  const [rotation, setRotation] = useState(0);
  const [offset, setOffset] = useState({ x: 0, y: 0 });
  const [natural, setNatural] = useState<{ width: number; height: number } | null>(null);
  const [dragging, setDragging] = useState(false);
  const [pinching, setPinching] = useState(false);
  const [fullscreen, setFullscreen] = useState(false);

  const stageRef = useRef<HTMLDivElement>(null);
  /** The whole viewer, so full screen keeps the toolbar reachable. */
  const rootRef = useRef<HTMLDivElement>(null);
  /** Live pointers, by id. One means drag; two means pinch. */
  const pointers = useRef(new Map<number, Point>());
  const dragStart = useRef({ x: 0, y: 0, ox: 0, oy: 0 });
  /** The pinch gesture in progress: its starting span, scale and offset. */
  const pinch = useRef<{
    distance: number;
    scale: number;
    ox: number;
    oy: number;
    mid: Point;
  } | null>(null);
  /** Scale and offset live in refs too, so a gesture reads the live value without
   *  the handlers being re-created on every frame of the gesture. `dragging` is a
   *  ref for the same reason: a touchscreen can deliver several pointermove events
   *  in one frame, and a state read would still be stale for the second one, which
   *  drops the movement instead of panning. */
  const scaleRef = useRef(scale);
  scaleRef.current = scale;
  const offsetRef = useRef(offset);
  offsetRef.current = offset;
  const draggingRef = useRef(false);

  /** Reset the view whenever the file changes. */
  useEffect(() => {
    setScale(1);
    setRotation(0);
    setOffset({ x: 0, y: 0 });
    setNatural(null);
    pointers.current.clear();
    pinch.current = null;
    draggingRef.current = false;
  }, [src]);

  const clampScale = (v: number) => Math.min(MAX_SCALE, Math.max(MIN_SCALE, v));

  /**
   * Scale by `factor`, keeping the point at (cx, cy) in the same place on
   * screen. Used by every zoom entry point: wheel, buttons, keys and pinch.
   */
  const zoomAt = useCallback((factor: number, cx?: number, cy?: number) => {
    setScale((prev) => {
      const next = clampScale(prev * factor);
      const stage = stageRef.current;
      if (!stage || cx === undefined || cy === undefined || next === prev) return next;
      const r = stage.getBoundingClientRect();
      const px = cx - (r.left + r.width / 2);
      const py = cy - (r.top + r.height / 2);
      setOffset((o) => ({
        x: o.x - px * (next / prev - 1),
        y: o.y - py * (next / prev - 1),
      }));
      return next;
    });
  }, []);

  /** Back to a fitted, upright view. There is one "fit" action, not a fit and a
   *  reset that happen to do the same thing — two buttons for one behaviour
   *  just makes one of them look broken. */
  const fit = useCallback(() => {
    setScale(1);
    setRotation(0);
    setOffset({ x: 0, y: 0 });
  }, []);

  /**
   * Toggle full screen on the whole viewer. Entering also fits the picture: a
   * view zoomed to a detail would otherwise land in full screen still cropped,
   * which is the opposite of what the button promises. Exiting leaves the view
   * alone, so the user's zoom survives the trip.
   *
   * The fit happens before the request is awaited, so it is applied at once
   * rather than after the browser has answered. A refused request then leaves
   * the view fitted, which is harmless.
   */
  const toggleFullscreen = useCallback(async () => {
    const el = rootRef.current;
    if (!el) return;
    if (document.fullscreenElement) {
      await document.exitFullscreen().catch(() => undefined);
    } else {
      fit();
      await el.requestFullscreen?.().catch(() => undefined);
    }
  }, [fit]);

  // Track the browser's own state, so the button reflects reality even when the
  // user leaves full screen with Esc or F11 rather than our control.
  useEffect(() => {
    const onChange = () => setFullscreen(Boolean(document.fullscreenElement));
    document.addEventListener('fullscreenchange', onChange);
    return () => document.removeEventListener('fullscreenchange', onChange);
  }, []);

  // Wheel zoom; passive:false so preventDefault actually stops page scroll.
  useEffect(() => {
    const stage = stageRef.current;
    if (!stage) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      zoomAt(e.deltaY < 0 ? 1.12 : 1 / 1.12, e.clientX, e.clientY);
    };
    stage.addEventListener('wheel', onWheel, { passive: false });
    return () => stage.removeEventListener('wheel', onWheel);
  }, [zoomAt]);

  // Keyboard shortcuts, only while the viewer owns the page. `f` is the key
  // every player uses for full screen.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === '+' || e.key === '=') zoomAt(1.2);
      else if (e.key === '-') zoomAt(1 / 1.2);
      else if (e.key === '0') fit();
      else if (e.key.toLowerCase() === 'r') setRotation((r) => (r + 90) % 360);
      else if (e.key.toLowerCase() === 'f') void toggleFullscreen();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [zoomAt, fit, toggleFullscreen]);

  /** The two live pointer positions, when exactly two are down. */
  const twoPointers = (): [Point, Point] | null => {
    if (pointers.current.size !== 2) return null;
    const [a, b] = [...pointers.current.values()] as [Point, Point];
    return [a, b];
  };

  const onPointerDown = (e: React.PointerEvent) => {
    pointers.current.set(e.pointerId, { x: e.clientX, y: e.clientY });
    // Capture so a finger that leaves the element still reports moves and the
    // matching release. A failure here is not fatal: the gesture just ends when
    // the pointer leaves.
    try {
      (e.currentTarget as Element).setPointerCapture(e.pointerId);
    } catch {
      /* no active pointer for this id; the gesture still works */
    }

    const pair = twoPointers();
    if (pair) {
      const [a, b] = pair;
      pinch.current = {
        distance: Math.hypot(a.x - b.x, a.y - b.y),
        scale: scaleRef.current,
        ox: offsetRef.current.x,
        oy: offsetRef.current.y,
        mid: { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 },
      };
      setPinching(true);
      draggingRef.current = false;
      setDragging(false);
      return;
    }

    // One pointer: pan, but only when there is something to pan.
    if (scaleRef.current > 1) {
      draggingRef.current = true;
      setDragging(true);
      dragStart.current = {
        x: e.clientX,
        y: e.clientY,
        ox: offsetRef.current.x,
        oy: offsetRef.current.y,
      };
    }
  };

  const onPointerMove = (e: React.PointerEvent) => {
    if (!pointers.current.has(e.pointerId)) return;
    pointers.current.set(e.pointerId, { x: e.clientX, y: e.clientY });

    const pair = twoPointers();
    if (pair && pinch.current) {
      const [a, b] = pair;
      const distance = Math.hypot(a.x - b.x, a.y - b.y);
      if (Math.abs(distance - pinch.current.distance) < PINCH_THRESHOLD_PX) return;

      const ratio = pinch.current.distance > 0 ? distance / pinch.current.distance : 1;
      const next = clampScale(pinch.current.scale * ratio);
      const mid = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };

      // Anchor the scale on the fingers' midpoint, then follow the midpoint as it
      // moves, so one gesture can zoom and pan together.
      const stage = stageRef.current;
      if (stage) {
        const r = stage.getBoundingClientRect();
        const px = pinch.current.mid.x - (r.left + r.width / 2);
        const py = pinch.current.mid.y - (r.top + r.height / 2);
        const grew = next / pinch.current.scale;
        setScale(next);
        setOffset({
          x: pinch.current.ox - px * (grew - 1) + (mid.x - pinch.current.mid.x),
          y: pinch.current.oy - py * (grew - 1) + (mid.y - pinch.current.mid.y),
        });
      }
      return;
    }

    if (!draggingRef.current) return;
    setOffset({
      x: dragStart.current.ox + (e.clientX - dragStart.current.x),
      y: dragStart.current.oy + (e.clientY - dragStart.current.y),
    });
  };

  const onPointerUp = (e: React.PointerEvent) => {
    pointers.current.delete(e.pointerId);
    try {
      (e.currentTarget as Element).releasePointerCapture?.(e.pointerId);
    } catch {
      /* already released */
    }

    // Lifting one finger of a pinch leaves a drag with the other; end the pinch
    // and rebase the drag so the image does not jump.
    if (pointers.current.size < 2) {
      pinch.current = null;
      setPinching(false);
    }
    if (pointers.current.size === 1 && scaleRef.current > 1) {
      const [p] = [...pointers.current.values()] as [Point];
      draggingRef.current = true;
      setDragging(true);
      dragStart.current = { x: p.x, y: p.y, ox: offsetRef.current.x, oy: offsetRef.current.y };
      return;
    }
    if (pointers.current.size === 0) {
      draggingRef.current = false;
      setDragging(false);
    }
  };

  return (
    <div ref={rootRef} className="imgviewer">
      <div className="imgbar">
        <Button size="sm" variant="ghost" onClick={() => zoomAt(1 / 1.2)} aria-label="Zoom out">
          <IconZoomOut />
        </Button>
        <span className="imgbar__zoom">{Math.round(scale * 100)}%</span>
        <Button size="sm" variant="ghost" onClick={() => zoomAt(1.2)} aria-label="Zoom in">
          <IconZoomIn />
        </Button>
        <span className="imgbar__sep" />
        <Button size="sm" variant="ghost" onClick={() => setRotation((r) => (r + 90) % 360)} aria-label="Rotate 90°">
          <IconRotate />
        </Button>
        <Button size="sm" variant="ghost" onClick={fit} aria-label="Fit to screen">
          <IconFit />
        </Button>
        <span className="imgbar__sep" />
        <Button
          size="sm"
          variant="ghost"
          onClick={() => void toggleFullscreen()}
          aria-label={fullscreen ? 'Exit full screen' : 'Full screen'}
          aria-pressed={fullscreen}
          title={fullscreen ? 'Exit full screen' : 'Full screen'}
        >
          {fullscreen ? <IconExitFullscreen /> : <IconFullscreen />}
        </Button>
        {natural && (
          <span className="imgbar__dims">
            {natural.width}×{natural.height}
          </span>
        )}
      </div>

      <div
        ref={stageRef}
        className={
          pinching ? 'imgstage is-pinching' : dragging ? 'imgstage is-dragging' : 'imgstage'
        }
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onPointerCancel={onPointerUp}
        onDoubleClick={() => (scale > 1 ? fit() : zoomAt(2))}
      >
        <img
          className="imgstage__img"
          src={src}
          alt={alt}
          draggable={false}
          onLoad={(e) => {
            const img = e.currentTarget;
            const dims = { width: img.naturalWidth, height: img.naturalHeight };
            setNatural(dims);
            onDimensions?.(dims);
          }}
          style={{
            transform: `translate(${offset.x}px, ${offset.y}px) scale(${scale}) rotate(${rotation}deg)`,
            cursor: scale > 1 ? (dragging ? 'grabbing' : 'grab') : 'default',
          }}
        />
      </div>
    </div>
  );
}
