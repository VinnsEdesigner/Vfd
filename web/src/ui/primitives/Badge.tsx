/**
 * Badge — status pill. Also carries a live "running" variant that pulses.
 *
 * The `tone` is a presentation concept, deliberately decoupled from the domain
 * state so a new state can be mapped to an existing tone without a CSS change.
 */

import type { ReactNode } from 'react';

export type BadgeTone = 'idle' | 'running' | 'done' | 'failed' | 'paused';

export interface BadgeProps {
  tone: BadgeTone;
  children: ReactNode;
}

export function Badge({ tone, children }: BadgeProps) {
  const cls = tone === 'idle' ? 'badge' : `badge badge--${tone}`;
  return <span className={cls}>{children}</span>;
}

export interface TagProps {
  children: ReactNode;
}

/** Neutral, monospace chip for metadata (format names, MIME types). */
export function Tag({ children }: TagProps) {
  return <span className="tag">{children}</span>;
}
