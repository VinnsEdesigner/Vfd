/**
 * VyzorStorageNotice — where downloads actually land, when Vyzor is local.
 *
 * This matters in a self-hosted setup. If Vyzor runs on the same device
 * (a phone running a Linux service, a desktop, a container), then downloads are
 * written by *Vyzor*, to a real directory on that device — not by the
 * browser, and not into browser storage at all.
 *
 * Saying so up front prevents the most likely confusion: the browser's own
 * storage options below are for keeping a *copy* for offline playback, while the
 * file itself already exists on disk at the path shown here. A browser cannot
 * report or choose that path — Vyzor owns it — so the honest thing is to
 * name it and stop implying otherwise.
 */

import { IconFolder } from '../primitives/Icons';
import { formatBytes } from '../../hooks/view-models';

export interface VyzorStorageFacts {
  /** Vyzor's work directory, as Vyzor sees it. */
  workDir: string;
  /** Mount point and filesystem, when Vyzor could resolve them. */
  mountPoint: string | null;
  fsType: string | null;
  /** Bytes Vyzor holds, and how many files. */
  libraryBytes: number;
  libraryFiles: number;
  /** Free space on that volume, when known. */
  freeBytes: number | null;
  /** True when Vyzor is answering on this same machine. */
  sameDevice: boolean;
}

export interface VyzorStorageNoticeProps {
  facts: VyzorStorageFacts;
  /** True when Vyzor's directory is somewhere the user can browse to. */
  browsable: boolean;
}

export function VyzorStorageNotice({ facts, browsable }: VyzorStorageNoticeProps) {
  return (
    <div className="engstore">
      <div className="engstore__head">
        <span className="engstore__icon">
          <IconFolder />
        </span>
        <div className="engstore__titles">
          <span className="engstore__title">
            {facts.sameDevice
              ? 'Downloads are saved on this device by Vyzor'
              : 'Downloads are saved on Vyzor’s machine'}
          </span>
          <span className="engstore__path" title={facts.workDir}>
            {facts.workDir}
          </span>
        </div>
      </div>

      <div className="engstore__facts">
        <span>
          {facts.libraryFiles} file{facts.libraryFiles === 1 ? '' : 's'} ·{' '}
          {formatBytes(facts.libraryBytes)}
        </span>
        {facts.freeBytes !== null && <span>{formatBytes(facts.freeBytes)} free</span>}
        {facts.mountPoint && (
          <span className="engstore__mono">
            {facts.mountPoint}
            {facts.fsType ? ` (${facts.fsType})` : ''}
          </span>
        )}
      </div>

      <p className="engstore__note">
        {browsable
          ? 'This path is on the device running Vyzor, so your other apps can open these files directly.'
          : 'Vyzor writes to this path itself. A browser cannot browse it or choose it — the browser storage options below only keep a separate copy for playing offline.'}
      </p>
    </div>
  );
}
