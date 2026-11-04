/**
 * Icons — small inline SVGs.
 *
 * Kept in one file because they share stroke conventions; a full icon library
 * would be overkill for the handful this console needs. Each renders at 1em
 * and inherits `currentColor`.
 */

import type { SVGProps } from 'react';

type IconProps = SVGProps<SVGSVGElement>;

const stroke = {
  fill: 'none',
  stroke: 'currentColor',
  strokeLinecap: 'round',
  strokeLinejoin: 'round',
} as const;

export function IconCheck(props: IconProps) {
  return (
    <svg viewBox="0 0 16 16" aria-hidden="true" {...props}>
      <path d="M3.5 8.5l3 3 6-7" {...stroke} strokeWidth={2} />
    </svg>
  );
}

export function IconX(props: IconProps) {
  return (
    <svg viewBox="0 0 16 16" aria-hidden="true" {...props}>
      <path d="M4 4l8 8M12 4l-8 8" {...stroke} strokeWidth={2} />
    </svg>
  );
}

export function IconPlus(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M10 4.5v11M4.5 10h11" {...stroke} strokeWidth={1.8} />
    </svg>
  );
}

export function IconRefresh(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M16.5 10a6.5 6.5 0 1 1-1.9-4.6M16.5 3v3.5H13" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconSun(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path
        d="M10 2.5v2M10 15.5v2M17.5 10h-2M4.5 10h-2M15.3 4.7l-1.4 1.4M6.1 13.9l-1.4 1.4M15.3 15.3l-1.4-1.4M6.1 6.1L4.7 4.7"
        {...stroke}
        strokeWidth={1.5}
      />
      <circle cx="10" cy="10" r="3.2" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconInfo(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <circle cx="10" cy="10" r="7.2" {...stroke} strokeWidth={1.5} />
      <path d="M10 9v4.5M10 6.6v.2" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconInbox(props: IconProps) {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" {...props}>
      <path
        d="M3 13l2.5-7A2 2 0 0 1 7.4 4.5h9.2a2 2 0 0 1 1.9 1.5L21 13v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"
        {...stroke}
        strokeWidth={1.6}
      />
      <path d="M3 13h5l1 2h6l1-2h5" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconFile(props: IconProps) {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" {...props}>
      <path d="M14 3v5h5" {...stroke} strokeWidth={1.6} />
      <path
        d="M6 3h8l5 5v13a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"
        {...stroke}
        strokeWidth={1.6}
      />
    </svg>
  );
}

export function IconPlay(props: IconProps) {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" {...props}>
      <path d="M8 5.5v13l11-6.5z" fill="currentColor" />
    </svg>
  );
}

export function IconFolder(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path
        d="M3 5.5A1.5 1.5 0 0 1 4.5 4h3l1.5 2h6A1.5 1.5 0 0 1 16.5 7.5v7A1.5 1.5 0 0 1 15 16H4.5A1.5 1.5 0 0 1 3 14.5z"
        {...stroke}
        strokeWidth={1.5}
      />
    </svg>
  );
}

export function IconCloud(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path
        d="M6 15h8.5a3 3 0 0 0 .4-6 4.5 4.5 0 0 0-8.7-1A3.5 3.5 0 0 0 6 15z"
        {...stroke}
        strokeWidth={1.5}
      />
    </svg>
  );
}

export function IconGrid(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M3 3h6v6H3zM11 3h6v4h-6zM11 9h6v8h-6zM3 11h6v6H3z" fill="currentColor" opacity={0.9} />
    </svg>
  );
}

export function IconList(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M3 5h14M3 10h14M3 15h9" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

/** A device: the copy saved on this phone/computer, not on the engine. */
export function IconDevice(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <rect x="5" y="2.5" width="10" height="15" rx="1.8" {...stroke} strokeWidth={1.5} />
      <path d="M8.5 15h3" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconDatabase(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <ellipse cx="10" cy="5" rx="6" ry="2.4" {...stroke} strokeWidth={1.5} />
      <path d="M4 5v10c0 1.3 2.7 2.4 6 2.4s6-1.1 6-2.4V5" {...stroke} strokeWidth={1.5} />
      <path d="M4 10c0 1.3 2.7 2.4 6 2.4s6-1.1 6-2.4" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconSettings(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <circle cx="10" cy="10" r="2.6" {...stroke} strokeWidth={1.5} />
      <path
        d="M10 2.6v2.2M10 15.2v2.2M17.4 10h-2.2M4.8 10H2.6M15.2 4.8l-1.6 1.6M6.4 13.6l-1.6 1.6M15.2 15.2l-1.6-1.6M6.4 6.4L4.8 4.8"
        {...stroke}
        strokeWidth={1.5}
      />
    </svg>
  );
}

export function IconLogo(props: IconProps) {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" {...props}>
      <rect x="2" y="2" width="20" height="20" rx="5" fill="currentColor" opacity={0.1} />
      <path d="M8 16V8l8 8V8" {...stroke} strokeWidth={2} />
    </svg>
  );
}

export function IconMore(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <circle cx="4.5" cy="10" r="1.6" fill="currentColor" />
      <circle cx="10" cy="10" r="1.6" fill="currentColor" />
      <circle cx="15.5" cy="10" r="1.6" fill="currentColor" />
    </svg>
  );
}

export function IconTrash(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M4 6h12M8 6V4.6c0-.9.7-1.6 1.6-1.6h.8c.9 0 1.6.7 1.6 1.6V6" {...stroke} strokeWidth={1.5} />
      <path d="M6 6l.7 9.1c.1 1 .9 1.7 1.9 1.7h2.8c1 0 1.8-.7 1.9-1.7L14 6" {...stroke} strokeWidth={1.5} />
      <path d="M9 9.5v4M11 9.5v4" {...stroke} strokeWidth={1.4} />
    </svg>
  );
}

export function IconExternal(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M11 4h5v5M16 4l-6 6" {...stroke} strokeWidth={1.6} />
      <path d="M15 11.5v3.1c0 .8-.6 1.4-1.4 1.4H5.4A1.4 1.4 0 0 1 4 14.6V6.4C4 5.6 4.6 5 5.4 5h3.1" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

/** A magnifier, for search affordances. */
export function IconSearch(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <circle cx="8.8" cy="8.8" r="5.3" {...stroke} strokeWidth={1.6} />
      <path d="M12.8 12.8L17 17" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconImage(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <rect x="3" y="4" width="14" height="12" rx="2" {...stroke} strokeWidth={1.5} />
      <circle cx="7.5" cy="8" r="1.3" fill="currentColor" />
      <path d="M4 14l3.8-3.8 3 3L13 11l3 3" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconMusic(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M8 13.5V6l8-1.6v7.1" {...stroke} strokeWidth={1.5} />
      <circle cx="6" cy="14" r="2.2" {...stroke} strokeWidth={1.5} />
      <circle cx="14" cy="12.5" r="2.2" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconPause(props: IconProps) {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" {...props}>
      <rect x="7" y="5" width="3.6" height="14" rx="1.1" fill="currentColor" />
      <rect x="13.4" y="5" width="3.6" height="14" rx="1.1" fill="currentColor" />
    </svg>
  );
}

export function IconVolume(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M4 8h2.6L10 5.2v9.6L6.6 12H4z" fill="currentColor" />
      <path d="M12.6 7.4a3.4 3.4 0 0 1 0 5.2M14.6 5.4a6 6 0 0 1 0 9.2" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconVolumeOff(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M4 8h2.6L10 5.2v9.6L6.6 12H4z" fill="currentColor" />
      <path d="M13 8l4 4M17 8l-4 4" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconSkipBack(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M15 5.5v9L8 10z" fill="currentColor" />
      <path d="M6 5.5v9" {...stroke} strokeWidth={1.7} />
    </svg>
  );
}

export function IconSkipForward(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M5 5.5v9L12 10z" fill="currentColor" />
      <path d="M14 5.5v9" {...stroke} strokeWidth={1.7} />
    </svg>
  );
}

export function IconZoomIn(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <circle cx="8.6" cy="8.6" r="5.1" {...stroke} strokeWidth={1.6} />
      <path d="M12.4 12.4L17 17M8.6 6.4v4.4M6.4 8.6h4.4" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconZoomOut(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <circle cx="8.6" cy="8.6" r="5.1" {...stroke} strokeWidth={1.6} />
      <path d="M12.4 12.4L17 17M6.4 8.6h4.4" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconRotate(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M16 10a6 6 0 1 1-1.8-4.3M16 3.5V7h-3.5" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

/* Fit: the picture sitting inside the frame that holds it. Deliberately not the
 * four corner brackets — that shape is the full-screen control, and two buttons
 * with the same glyph in one toolbar read as the same button. */
export function IconFit(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <rect x="3" y="4" width="14" height="12" rx="1.8" {...stroke} strokeWidth={1.5} />
      <rect x="6.6" y="7.2" width="6.8" height="5.6" rx="0.9" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconReset(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M4 10a6 6 0 1 0 1.8-4.3M4 3.5V7h3.5" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconPip(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <rect x="2.5" y="4.5" width="15" height="11" rx="1.8" {...stroke} strokeWidth={1.5} />
      <rect x="10" y="9" width="6" height="5" rx="1" fill="currentColor" />
    </svg>
  );
}

export function IconFullscreen(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M3.5 7.5V3.5h4M16.5 7.5V3.5h-4M3.5 12.5v4h4M16.5 12.5v4h-4" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconExitFullscreen(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M7.5 3.5v4h-4M12.5 3.5v4h4M7.5 16.5v-4h-4M12.5 16.5v-4h4" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconFilter(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M3 5h14l-5.4 6.2V16l-3.2-1.6V11.2z" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconSort(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M6 4v12M3 13l3 3 3-3M14 16V4M11 7l3-3 3 3" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconShare(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <circle cx="15" cy="5" r="2.2" {...stroke} strokeWidth={1.5} />
      <circle cx="5" cy="10" r="2.2" {...stroke} strokeWidth={1.5} />
      <circle cx="15" cy="15" r="2.2" {...stroke} strokeWidth={1.5} />
      <path d="M7 9l6-3M7 11l6 3" {...stroke} strokeWidth={1.5} />
    </svg>
  );
}

export function IconMove(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h3l1.5 2h6A1.5 1.5 0 0 1 16.5 8.5v6A1.5 1.5 0 0 1 15 16H4.5A1.5 1.5 0 0 1 3 14.5z" {...stroke} strokeWidth={1.5} />
      <path d="M10 9.5v4M8 11.5l2 2 2-2" {...stroke} strokeWidth={1.4} />
    </svg>
  );
}

export function IconDownload(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M10 3.5v8M6.8 8.6L10 11.8l3.2-3.2" {...stroke} strokeWidth={1.6} />
      <path d="M4 14.5h12" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconArrowLeft(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M11.5 5.5L7 10l4.5 4.5" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconArrowRight(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M8.5 5.5L13 10l-4.5 4.5" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconChevronDown(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M6 8.5l4 4 4-4" {...stroke} strokeWidth={1.6} />
    </svg>
  );
}

export function IconMinus(props: IconProps) {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true" {...props}>
      <path d="M5 10h10" {...stroke} strokeWidth={1.8} />
    </svg>
  );
}
