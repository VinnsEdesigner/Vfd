/**
 * BootGate — holds the app behind a splash until the first data load settles.
 *
 * A blank screen while the first request is in flight reads as "broken"; a
 * spinner reads as "working". It also prevents the shell from painting empty
 * panels that immediately get replaced.
 */

import type { ReactNode } from 'react';
import { Spinner } from './primitives/Spinner';

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
        {/* The app mark itself, so the splash and the browser tab agree. */}
        <img className="boot__mark" src="/logo.svg" alt="" width={72} height={72} />
        <span className="boot__title">{label}</span>
        <Spinner size="lg" />
      </div>
    </div>
  );
}
