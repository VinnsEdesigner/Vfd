/**
 * BootGate — holds the app behind a splash until the first data load settles.
 *
 * A blank screen while the first request is in flight reads as "broken"; a
 * spinner reads as "working". It also prevents the shell from painting empty
 * panels that immediately get replaced.
 */

import type { ReactNode } from 'react';
import { Spinner } from './primitives/Spinner';
import { IconLogo } from './primitives/Icons';

export interface BootGateProps {
  ready: boolean;
  label?: string;
  children: ReactNode;
}

export function BootGate({ ready, label = 'VYZORIX FILE DOWNLOADER', children }: BootGateProps) {
  if (ready) return <>{children}</>;

  return (
    <div className="boot" role="status" aria-live="polite">
      <div className="boot__inner">
        <IconLogo className="boot__mark" />
        <Spinner size="lg" />
        <span className="boot__label">{label}</span>
      </div>
    </div>
  );
}
