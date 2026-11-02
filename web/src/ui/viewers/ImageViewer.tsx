/**
 * ImageViewer — full-page image with zoom, pan, rotate and fit.
 *
 * Zoom is anchored at the cursor so wheeling in on a detail keeps that detail
 * under the pointer, which is what people expect from an image viewer. Pan is
 * only enabled once the image overflows, otherwise dragging does nothing and
 * feels broken.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { Button } from '../primitives/Button';
import { IconZoomIn, IconZoomOut, IconRotate, IconFit, IconReset } from '../primitives/Icons';

export interface ImageViewerProps {
  src: string;
  alt: string;
  onDimensions?: (dims: { width: number; height: number }) => void;
}

const MIN_SCALE = 0.1;
const MAX_SCALE = 8;

export function ImageViewer({ src, alt, onDimensions }: ImageViewerProps) {
  const [scale, setScale] = useState(1);
  const [rotation, setRotation] = useState(0);
  const [offset, setOffset] = useState({ x: 0, y: 0 });
  const [natural, setNatural] = useState<{ width: number; height: number } | null>(null);
  const [dragging, setDragging] = useState(false);

  const stageRef = useRef<HTMLDivElement>(null);
  const dragStart = useRef({ x: 0, y: 0, ox: 0, oy: 0 });

  /** Reset the view whenever the file changes. */
  useEffect(() => {
    setScale(1);
    setRotation(0);
    setOffset({ x: 0, y: 0 });
    setNatural(null);
  }, [src]);

  const clampScale = (v: number) => Math.min(MAX_SCALE, Math.max(MIN_SCALE, v));

  const zoomAt = useCallback((factor: number, cx?: number, cy?: number) => {
    setScale((prev) => {
      const next = clampScale(prev * factor);
      const stage = stageRef.current;
      if (!stage || cx === undefined || cy === undefined) return next;
      const r = stage.getBoundingClientRect();
      // Keep the point under the cursor stationary while the scale changes.
      const px = cx - (r.left + r.width / 2);
      const py = cy - (r.top + r.height / 2);
      setOffset((o) => ({
        x: o.x - px * (next / prev - 1),
        y: o.y - py * (next / prev - 1),
      }));
      return next;
    });
  }, []);

  const reset = useCallback(() => {
    setScale(1);
    setRotation(0);
    setOffset({ x: 0, y: 0 });
  }, []);

  const fit = useCallback(() => {
    reset();
  }, [reset]);

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

  // Keyboard shortcuts, only while the viewer owns the page.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === '+' || e.key === '=') zoomAt(1.2);
      else if (e.key === '-') zoomAt(1 / 1.2);
      else if (e.key === '0') fit();
      else if (e.key.toLowerCase() === 'r') setRotation((r) => (r + 90) % 360);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [zoomAt, fit]);

  const onPointerDown = (e: React.PointerEvent) => {
    if (scale <= 1) return;
    setDragging(true);
    dragStart.current = { x: e.clientX, y: e.clientY, ox: offset.x, oy: offset.y };
    (e.target as Element).setPointerCapture(e.pointerId);
  };
  const onPointerMove = (e: React.PointerEvent) => {
    if (!dragging) return;
    setOffset({
      x: dragStart.current.ox + (e.clientX - dragStart.current.x),
      y: dragStart.current.oy + (e.clientY - dragStart.current.y),
    });
  };
  const onPointerUp = (e: React.PointerEvent) => {
    setDragging(false);
    (e.target as Element).releasePointerCapture?.(e.pointerId);
  };

  return (
    <>
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
        <Button size="sm" variant="ghost" onClick={reset} aria-label="Reset view">
          <IconReset />
        </Button>
        {natural && (
          <span className="imgbar__dims">
            {natural.width}×{natural.height}
          </span>
        )}
      </div>

      <div
        ref={stageRef}
        className={dragging ? 'imgstage is-dragging' : 'imgstage'}
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
    </>
  );
}
