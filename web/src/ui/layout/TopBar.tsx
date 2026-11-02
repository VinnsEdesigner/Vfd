/**
 * TopBar — the fixed page header. Lives in the shell's first grid row, so it
 * cannot scroll away with the content.
 */

import type { ReactNode } from 'react';

export interface TopBarProps {
  title: string;
  subtitle?: string;
  /** Buttons rendered at the right. */
  actions?: ReactNode;
}

export function TopBar({ title, subtitle, actions }: TopBarProps) {
  return (
    <header className="topbar">
      <div className="topbar__left">
        <h1>{title}</h1>
        {subtitle !== undefined && <p>{subtitle}</p>}
      </div>
      {actions !== undefined && <div className="topbar__right">{actions}</div>}
    </header>
  );
}
