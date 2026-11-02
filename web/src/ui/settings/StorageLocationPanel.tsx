/**
 * StorageLocationPanel — choose where downloads are saved.
 *
 * The choice is functional, not cosmetic: it changes which fields are shown and
 * the path displayed on every file row. `target` is controlled by the parent so
 * it can live in a store and persist.
 */

import { Card } from '../primitives/Card';
import { Field } from '../primitives/Field';
import { SwitchRow } from '../primitives/SwitchRow';
import { Button } from '../primitives/Button';
import { AppIcon } from '../primitives/AppIcon';
import { AppIconDevice, AppIconCloud } from '../primitives/AppIcons';
import type { StorageTarget } from '../files/FileTable';

export interface StorageLocationValue {
  target: StorageTarget;
  localPath: string;
  localPartsPath: string;
  openFolderOnFinish: boolean;
  bucket: string;
  keyPrefix: string;
  serveViaCdn: boolean;
}

export interface StorageLocationPanelProps {
  value: StorageLocationValue;
  onChange: (next: StorageLocationValue) => void;
  onSave?: () => void;
  onTest?: () => void;
  /** Which action is in flight, so its button can spin. */
  busy?: string | null;
  /** Disk figures, already formatted. */
  usage?: { library: string; transferred: string; free: string; usedPercent: number; summary: string };
}

export function StorageLocationPanel({
  value,
  onChange,
  onSave,
  onTest,
  busy = null,
  usage,
}: StorageLocationPanelProps) {
  const isLocal = value.target === 'local';
  const set = (patch: Partial<StorageLocationValue>) => onChange({ ...value, ...patch });

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
                Save to the local filesystem. Finished files can be opened in your player.
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
                value={value.localPath}
                onChange={(localPath) => set({ localPath })}
                hint="Created automatically if it does not exist."
              />
              <Field
                label="Temporary parts folder"
                value={value.localPartsPath}
                onChange={(localPartsPath) => set({ localPartsPath })}
                hint="Ranged downloads are assembled here before moving."
              />
            </div>
            <div style={{ marginTop: 18 }}>
              <SwitchRow
                name="Open folder when a download finishes"
                description="Reveal the file in Finder or Explorer"
                checked={value.openFolderOnFinish}
                onChange={(openFolderOnFinish) => set({ openFolderOnFinish })}
              />
            </div>
          </>
        ) : (
          <>
            <div className="form" style={{ marginTop: 22 }}>
              <Field
                label="Bucket"
                value={value.bucket}
                onChange={(bucket) => set({ bucket })}
              />
              <Field
                label="Key prefix"
                value={value.keyPrefix}
                onChange={(keyPrefix) => set({ keyPrefix })}
              />
            </div>
            <div style={{ marginTop: 18 }}>
              <SwitchRow
                name="Serve files through the CDN"
                description="Return a public URL instead of a local path"
                checked={value.serveViaCdn}
                onChange={(serveViaCdn) => set({ serveViaCdn })}
              />
            </div>
          </>
        )}

        <div className="form-actions">
          <Button variant="primary" loading={busy === 'save-location'} onClick={onSave}>
            Save location
          </Button>
          <Button loading={busy === 'test-location'} onClick={onTest}>
            Test write access
          </Button>
        </div>
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
