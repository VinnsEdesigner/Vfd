/**
 * StorageLocationPanel — where Vyzor writes downloads, and how to change it.
 *
 * The directory belongs to Vyzor, not the browser: it is a real path on the
 * Vyzor's filesystem, chosen at startup and changeable at runtime. So this panel
 * reads Vyzor's current directory and volume, and saving a new one is a real
 * request Vyzor validates by writing to it.
 *
 * The device/cloud toggle is a display choice — it decides whether file rows show
 * a local path or an object-storage URL — and is kept client-side.
 */

import { useEffect, useState } from 'react';
import { Card } from '../primitives/Card';
import { Field } from '../primitives/Field';
import { Button } from '../primitives/Button';
import { AppIcon } from '../primitives/AppIcon';
import { AppIconDevice, AppIconCloud } from '../primitives/AppIcons';
import type { StorageTarget } from '../files';

export interface StorageLocationValue {
  target: StorageTarget;
  bucket: string;
  keyPrefix: string;
}

/** What Vyzor reports about the directory it writes to. */
export interface VyzorLocationFacts {
  workDir: string;
  writable: boolean;
  writeError: string | null;
  /** e.g. "/" or "/srv" — null when Vyzor could not resolve it. */
  mountPoint: string | null;
  fsType: string | null;
}

export interface StorageLocationPanelProps {
  value: StorageLocationValue;
  onChange: (next: StorageLocationValue) => void;
  /** Vyzor's current directory, or null while it is still loading. */
  vyzorLocation: VyzorLocationFacts | null;
  onSave?: (dir: string) => void;
  busy?: string | null;
  /** Disk figures, already formatted. */
  usage?: { library: string; transferred: string; free: string; usedPercent: number; summary: string };
}

export function StorageLocationPanel({
  value,
  onChange,
  vyzorLocation,
  onSave,
  busy = null,
  usage,
}: StorageLocationPanelProps) {
  const isLocal = value.target === 'local';
  const set = (patch: Partial<StorageLocationValue>) => onChange({ ...value, ...patch });

  // The input starts at Vyzor's current directory and re-syncs whenever the
  // Vyzor reports a different one (a save that succeeded, or another client).
  const [dir, setDir] = useState(vyzorLocation?.workDir ?? '');
  const vyzorDir = vyzorLocation?.workDir ?? '';
  useEffect(() => {
    if (vyzorLocation) setDir(vyzorLocation.workDir);
  }, [vyzorLocation, vyzorDir]);

  const dirty = vyzorLocation ? dir.trim() !== vyzorLocation.workDir : false;
  const canSave = Boolean(dir.trim()) && dirty;

  const volume =
    vyzorLocation && vyzorLocation.mountPoint
      ? `${vyzorLocation.mountPoint}${vyzorLocation.fsType ? ` (${vyzorLocation.fsType})` : ''}`
      : null;

  return (
    <>
      <Card
        title="Where downloads are saved"
        subtitle="Applies to new downloads. Existing files stay where they are."
      >
        <div className="choice-grid">
          <label className={isLocal ? 'choice is-active' : 'choice'}>
            <input
              type="radio"
              name="storage-target"
              value="local"
              checked={isLocal}
              onChange={() => set({ target: 'local' })}
            />
            <AppIcon size="xxl">
              <AppIconDevice />
            </AppIcon>
            <span className="choice__body">
              <span className="choice__name">This device</span>
              <span className="choice__desc">
                Save to Vyzor&rsquo;s filesystem. Finished files can be opened in your player.
              </span>
            </span>
            <span className="choice__radio" />
          </label>

          <label className={!isLocal ? 'choice is-active' : 'choice'}>
            <input
              type="radio"
              name="storage-target"
              value="cloud"
              checked={!isLocal}
              onChange={() => set({ target: 'cloud' })}
            />
            <AppIcon size="xxl">
              <AppIconCloud />
            </AppIcon>
            <span className="choice__body">
              <span className="choice__name">Cloud storage</span>
              <span className="choice__desc">
                Upload straight to the configured bucket and serve through the CDN.
              </span>
            </span>
            <span className="choice__radio" />
          </label>
        </div>

        {isLocal ? (
          <>
            <div className="form" style={{ marginTop: 22 }}>
              <Field
                label="Download folder"
                value={dir}
                onChange={setDir}
                hint={
                  vyzorLocation
                    ? vyzorLocation.writable
                      ? 'A directory on the machine running Vyzor. Created if it does not exist.'
                      : `Vyzor cannot write here${
                          vyzorLocation.writeError ? `: ${vyzorLocation.writeError}` : ''
                        }.`
                    : 'Loading Vyzor\u2019s directory\u2026'
                }
              />
            </div>
            {volume && (
              <div className="field__hint" style={{ marginTop: 4 }}>
                On the {volume} volume.
              </div>
            )}
          </>
        ) : (
          <div className="form" style={{ marginTop: 22 }}>
            <Field label="Bucket" value={value.bucket} onChange={(bucket) => set({ bucket })} />
            <Field
              label="Key prefix"
              value={value.keyPrefix}
              onChange={(keyPrefix) => set({ keyPrefix })}
              hint="Prefixes the object key for every upload."
            />
          </div>
        )}

        {isLocal && (
          <div className="form-actions">
            <Button
              variant="primary"
              loading={busy === 'save-location'}
              disabled={!canSave}
              onClick={() => onSave?.(dir.trim())}
            >
              Save location
            </Button>
          </div>
        )}
      </Card>

      {usage && (
        <Card title="Disk usage">
          <div className="usage">
            <div className="usage__row">
              <span>Ingest library</span>
              <span className="num">{usage.library}</span>
            </div>
            <div className="usage__row">
              <span>Bytes transferred</span>
              <span className="num">{usage.transferred}</span>
            </div>
            <div className="usage__row">
              <span>Free space</span>
              <span className="num">{usage.free}</span>
            </div>
          </div>
          <div className="usage__bar">
            <span style={{ width: `${usage.usedPercent}%` }} />
          </div>
          <div className="field__hint" style={{ marginTop: 8 }}>
            {usage.summary}
          </div>
        </Card>
      )}
    </>
  );
}
