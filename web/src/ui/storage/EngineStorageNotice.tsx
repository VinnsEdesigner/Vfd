/**
 * EngineStorageNotice — where downloads actually land, when the engine is local.
 *
 * This matters in a self-hosted setup. If the engine runs on the same device
 * (a phone running a Linux service, a desktop, a container), then downloads are
 * written by *the engine*, to a real directory on that device — not by the
 * browser, and not into browser storage at all.
 *
 * Saying so up front prevents the most likely confusion: the browser's own
 * storage options below are for keeping a *copy* for offline playback, while the
 * file itself already exists on disk at the path shown here. A browser cannot
 * report or choose that path — the engine owns it — so the honest thing is to
 * name it and stop implying otherwise.
 */

import { IconFolder } from '../primitives/Icons';
import { formatBytes } from '../../hooks/view-models';

export interface EngineStorageFacts {
  /** The engine's work directory, as the engine sees it. */
  workDir: string;
  /** Mount point and filesystem, when the engine could resolve them. */
  mountPoint: string | null;
  fsType: string | null;
  /** Bytes the engine holds, and how many files. */
  libraryBytes: number;
  libraryFiles: number;
  /** Free space on that volume, when known. */
  freeBytes: number | null;
  /** True when the engine is answering on this same machine. */
  sameDevice: boolean;
}

export interface EngineStorageNoticeProps {
  facts: EngineStorageFacts;
  /** True when the engine's directory is somewhere the user can browse to. */
  browsable: boolean;
}

export function EngineStorageNotice({ facts, browsable }: EngineStorageNoticeProps) {
  return (
    <div className="engstore">
      <div className="engstore__head">
        <span className="engstore__icon">
          <IconFolder />
        </span>
        <div className="engstore__titles">
          <span className="engstore__title">
            {facts.sameDevice
              ? 'Downloads are saved on this device by the engine'
              : 'Downloads are saved on the engine’s machine'}
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
          ? 'This path is on the device running the engine, so your other apps can open these files directly.'
          : 'The engine writes to this path itself. A browser cannot browse it or choose it — the browser storage options below only keep a separate copy for playing offline.'}
      </p>
    </div>
  );
}
