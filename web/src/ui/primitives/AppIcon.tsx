/**
 * AppIcon — a rounded mask for an illustrative app icon.
 *
 * The icons in `AppIcons.tsx` are self-contained: each draws its own gradient
 * plate edge to edge. So this wrapper no longer tints or backgrounds anything —
 * it just clips the square SVG to a squircle and gives it the elevation that
 * makes an app icon sit on the surface.
 */

import type { ReactNode } from 'react';

export type AppIconSize = 'sm' | 'md' | 'lg' | 'xl' | 'xxl';

export interface AppIconProps {
  children: ReactNode;
  size?: AppIconSize;
  className?: string;
  /** Drop the shadow — useful when the icon sits on a tinted surface. */
  flat?: boolean;
}

export function AppIcon({ children, size = 'md', className, flat = false }: AppIconProps) {
  const cls = ['appicon', `appicon--${size}`, flat ? 'appicon--flat' : '', className]
    .filter(Boolean)
    .join(' ');
  return (
    <span className={cls} aria-hidden="true">
      {children}
    </span>
  );
}
