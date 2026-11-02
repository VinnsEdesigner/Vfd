/**
 * Skeleton — shimmering placeholder blocks used while a page's data loads.
 *
 * `Skeleton` is the primitive; the page-shaped compositions live next to each
 * page so a skeleton always matches the layout it stands in for.
 */

import type { CSSProperties, ReactNode } from 'react';

export interface SkeletonProps {
  width?: string | number;
  height?: string | number;
  radius?: string;
  className?: string;
}

export function Skeleton({ width, height, radius, className }: SkeletonProps) {
  const style: CSSProperties = {};
  if (width !== undefined) style.width = typeof width === 'number' ? `${width}px` : width;
  if (height !== undefined) style.height = typeof height === 'number' ? `${height}px` : height;
  if (radius !== undefined) style.borderRadius = radius;
  return <span className={['skeleton', className].filter(Boolean).join(' ')} style={style} />;
}

/** A block of stacked lines, for paragraph-ish content. */
export function SkeletonLines({ lines = 3 }: { lines?: number }) {
  return (
    <div className="skeleton-lines">
      {Array.from({ length: lines }, (_, i) => (
        <Skeleton key={i} height={12} width={i === lines - 1 ? '62%' : '100%'} />
      ))}
    </div>
  );
}

/** A skeleton that fills a table body with plausible rows. */
export function SkeletonRows({
  rows = 5,
  columns = 6,
}: {
  rows?: number;
  columns?: number;
}) {
  return (
    <>
      {Array.from({ length: rows }, (_, r) => (
        <tr key={r} className="skeleton-row">
          {Array.from({ length: columns }, (_, c) => (
            <td key={c}>
              <Skeleton height={12} width={c === 1 ? '80%' : '58%'} />
            </td>
          ))}
        </tr>
      ))}
    </>
  );
}

export interface SkeletonPanelProps {
  children: ReactNode;
}

/** Wraps skeleton content in the standard card chrome. */
export function SkeletonPanel({ children }: SkeletonPanelProps) {
  return <div className="card">{children}</div>;
}
