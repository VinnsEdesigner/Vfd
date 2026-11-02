/**
 * Note — inline advisory text with an info glyph.
 */

import type { ReactNode } from 'react';
import { IconInfo } from './Icons';

export interface NoteProps {
  children: ReactNode;
  className?: string;
}

export function Note({ children, className }: NoteProps) {
  return (
    <div className={['note', className].filter(Boolean).join(' ')}>
      <IconInfo />
      <span>{children}</span>
    </div>
  );
}
