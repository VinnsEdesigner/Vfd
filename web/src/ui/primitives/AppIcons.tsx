/**
 * AppIcons — illustrative, app-store-style icons.
 *
 * Each icon is self-contained: its own gradient background plate, a layered
 * foreground with highlight and shadow, and (where it reads better) a small
 * secondary element for depth. They fill their container edge to edge, so the
 * `AppIcon` wrapper is just a rounded mask.
 *
 * Gradient ids must be unique per instance — two icons sharing `id="g"` would
 * make the second silently adopt the first's paint. `useId()` gives us that for
 * free and is stable across renders.
 */

import { useId, type SVGProps } from 'react';

type IconProps = Omit<SVGProps<SVGSVGElement>, 'id'>;

/** Shared frame: 48×48 plate, gradient background, soft inner highlight. */
function Plate({
  bgId,
  from,
  to,
  children,
  highlight = true,
}: {
  bgId: string;
  from: string;
  to: string;
  children: React.ReactNode;
  highlight?: boolean;
}) {
  return (
    <>
      <defs>
        <linearGradient id={bgId} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor={from} />
          <stop offset="1" stopColor={to} />
        </linearGradient>
      </defs>
      <rect width="48" height="48" rx="11" fill={`url(#${bgId})`} />
      {highlight && (
        // Top-edge sheen: the cheap trick that makes a flat plate read as glass.
        <path
          d="M0 11C0 4.9 4.9 0 11 0h26c6.1 0 11 4.9 11 11v3c-8 4-16 6-24 6S8 18 0 14z"
          fill="#fff"
          opacity="0.16"
        />
      )}
      {children}
    </>
  );
}

/* ------------------------------------------------------------------ */
/* Downloads — an arrow dropping into a tray                           */
/* ------------------------------------------------------------------ */

export function AppIconDownload(props: IconProps) {
  const uid = useId();
  return (
    <svg viewBox="0 0 48 48" aria-hidden="true" {...props}>
      <Plate bgId={`${uid}-bg`} from="#fb7185" to="#be123c">
        <defs>
          <linearGradient id={`${uid}-arrow`} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stopColor="#ffffff" />
            <stop offset="1" stopColor="#ffe4e6" />
          </linearGradient>
        </defs>
        {/* tray */}
        <path
          d="M11 29.5a2.6 2.6 0 0 1 2.6 2.6v2.3c0 .7.6 1.3 1.3 1.3h18.2c.7 0 1.3-.6 1.3-1.3v-2.3a2.6 2.6 0 1 1 5.2 0v2.3a6.5 6.5 0 0 1-6.5 6.5H14.9a6.5 6.5 0 0 1-6.5-6.5v-2.3a2.6 2.6 0 0 1 2.6-2.6z"
          fill="#fff"
          opacity="0.42"
        />
        {/* arrow */}
        <path
          d="M24 8.5c1.5 0 2.7 1.2 2.7 2.7v13.1l3.6-3.6a2.7 2.7 0 0 1 3.9 3.8l-8.3 8.3a2.7 2.7 0 0 1-3.8 0l-8.3-8.3a2.7 2.7 0 0 1 3.9-3.8l3.6 3.6V11.2c0-1.5 1.2-2.7 2.7-2.7z"
          fill={`url(#${uid}-arrow)`}
        />
      </Plate>
    </svg>
  );
}

/* ------------------------------------------------------------------ */
/* Storage — stacked drive bays with activity LEDs                     */
/* ------------------------------------------------------------------ */

export function AppIconStorage(props: IconProps) {
  const uid = useId();
  return (
    <svg viewBox="0 0 48 48" aria-hidden="true" {...props}>
      <Plate bgId={`${uid}-bg`} from="#475569" to="#1e293b">
        {/* top bay, recessed */}
        <rect x="9" y="11" width="30" height="11" rx="3" fill="#0f172a" opacity="0.55" />
        <rect x="9" y="11" width="30" height="10" rx="3" fill="#e2e8f0" />
        <rect x="9" y="11" width="30" height="4" rx="3" fill="#fff" opacity="0.5" />
        <circle cx="34.5" cy="16" r="1.7" fill="#22c55e" />
        <rect x="13" y="14.6" width="13" height="1.8" rx="0.9" fill="#94a3b8" />
        {/* bottom bay */}
        <rect x="9" y="26" width="30" height="11" rx="3" fill="#0f172a" opacity="0.55" />
        <rect x="9" y="26" width="30" height="10" rx="3" fill="#f1f5f9" />
        <rect x="9" y="26" width="30" height="4" rx="3" fill="#fff" opacity="0.55" />
        <circle cx="34.5" cy="31" r="1.7" fill="#fbbf24" />
        <rect x="13" y="29.6" width="9" height="1.8" rx="0.9" fill="#94a3b8" />
      </Plate>
    </svg>
  );
}

/* ------------------------------------------------------------------ */
/* This device — a phone showing a playing video                       */
/* ------------------------------------------------------------------ */

export function AppIconDevice(props: IconProps) {
  const uid = useId();
  return (
    <svg viewBox="0 0 48 48" aria-hidden="true" {...props}>
      <Plate bgId={`${uid}-bg`} from="#818cf8" to="#4338ca">
        {/* body */}
        <rect x="14" y="7" width="20" height="34" rx="4.5" fill="#0f172a" opacity="0.35" />
        <rect x="14" y="7" width="20" height="33" rx="4.5" fill="#f8fafc" />
        {/* screen */}
        <rect x="16.4" y="10.4" width="15.2" height="24.2" rx="2.6" fill="#1e1b4b" />
        {/* play badge on the screen */}
        <circle cx="24" cy="22.5" r="5.4" fill="#fff" opacity="0.95" />
        <path d="M22.4 19.6l4.4 2.9-4.4 2.9z" fill="#4338ca" />
        {/* speaker slot */}
        <rect x="21" y="8.7" width="6" height="1.2" rx="0.6" fill="#cbd5e1" />
        {/* home indicator */}
        <rect x="21" y="36.6" width="6" height="1.3" rx="0.65" fill="#cbd5e1" />
      </Plate>
    </svg>
  );
}

/* ------------------------------------------------------------------ */
/* Cloud storage — a cloud with an upload arrow                        */
/* ------------------------------------------------------------------ */

export function AppIconCloud(props: IconProps) {
  const uid = useId();
  return (
    <svg viewBox="0 0 48 48" aria-hidden="true" {...props}>
      <Plate bgId={`${uid}-bg`} from="#38bdf8" to="#1d4ed8">
        {/* cloud */}
        <path
          d="M15.5 35.5a8.5 8.5 0 0 1-1-16.9 11.5 11.5 0 0 1 21.8-2.6 8 8 0 0 1-1.4 15.9z"
          fill="#fff"
          opacity="0.95"
        />
        <path
          d="M15.5 35.5a8.5 8.5 0 0 1-1-16.9 11.5 11.5 0 0 1 21.8-2.6 8 8 0 0 1-1.4 15.9z"
          fill="#fff"
          opacity="0.25"
          transform="translate(0,1.5)"
        />
        {/* arrow punched through the cloud */}
        <circle cx="24" cy="27.5" r="6.6" fill="#1d4ed8" opacity="0.9" />
        <path
          d="M24 22.2c.9 0 1.6.7 1.6 1.6v4.4l1.7-1.7a1.6 1.6 0 0 1 2.3 2.3l-4.5 4.5a1.6 1.6 0 0 1-2.2 0l-4.5-4.5a1.6 1.6 0 1 1 2.3-2.3l1.7 1.7v-4.4c0-.9.7-1.6 1.6-1.6z"
          fill="#fff"
        />
      </Plate>
    </svg>
  );
}

/* ------------------------------------------------------------------ */
/* Video file — a document with a play triangle and a duration strip   */
/* ------------------------------------------------------------------ */

export function AppIconFile(props: IconProps) {
  const uid = useId();
  return (
    <svg viewBox="0 0 48 48" aria-hidden="true" {...props}>
      <Plate bgId={`${uid}-bg`} from="#fbbf24" to="#b45309">
        {/* page */}
        <path d="M15 8h12l8 8v24a3 3 0 0 1-3 3H15a3 3 0 0 1-3-3V11a3 3 0 0 1 3-3z" fill="#fffbeb" />
        {/* folded corner */}
        <path d="M27 8l8 8h-6a2 2 0 0 1-2-2z" fill="#fcd34d" />
        {/* play badge */}
        <circle cx="24" cy="26" r="7.2" fill="#b45309" opacity="0.16" />
        <circle cx="24" cy="25.4" r="7.2" fill="#b45309" />
        <path d="M22 21.9l6 3.5-6 3.5z" fill="#fffbeb" />
        {/* duration strip */}
        <rect x="16" y="35.4" width="16" height="2.6" rx="1.3" fill="#b45309" opacity="0.35" />
        <rect x="16" y="35.4" width="9" height="2.6" rx="1.3" fill="#b45309" opacity="0.7" />
      </Plate>
    </svg>
  );
}

/* ------------------------------------------------------------------ */
/* Pipeline — ordered stages with a live one                           */
/* ------------------------------------------------------------------ */

export function AppIconPipeline(props: IconProps) {
  const uid = useId();
  return (
    <svg viewBox="0 0 48 48" aria-hidden="true" {...props}>
      <Plate bgId={`${uid}-bg`} from="#34d399" to="#047857">
        <rect x="10" y="11" width="28" height="9" rx="3" fill="#ecfdf5" />
        <circle cx="15" cy="15.5" r="2" fill="#059669" />
        <rect x="20" y="14.6" width="13" height="1.9" rx="0.95" fill="#a7f3d0" />
        <rect x="10" y="23" width="28" height="9" rx="3" fill="#fff" />
        <circle cx="15" cy="27.5" r="2" fill="#f59e0b" />
        <rect x="20" y="26.6" width="13" height="1.9" rx="0.95" fill="#cbd5e1" />
        <rect x="10" y="35" width="28" height="6" rx="3" fill="#065f46" opacity="0.55" />
      </Plate>
    </svg>
  );
}
