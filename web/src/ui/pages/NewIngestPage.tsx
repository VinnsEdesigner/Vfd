/**
 * NewIngestPage — the source form, pipeline step switches, and the probe panel.
 *
 * The probe result is passed in already-shaped so this page stays presentational;
 * the caller owns the request.
 */

import { Card } from '../primitives/Card';
import { Field } from '../primitives/Field';
import { SwitchRow } from '../primitives/SwitchRow';
import { Button } from '../primitives/Button';
import { Note } from '../primitives/Note';
import { QualityPicker } from '../downloads/QualityPicker';
import type { QualityOption } from '../../hooks/view-models';

export interface IngestFormValue {
  url: string;
  connections: number;
  saveAs: string;
  hls: boolean;
  upload: boolean;
  purge: boolean;
  /** Runtime of the title, in seconds. Used to derive real sizes from a
   *  manifest's declared bandwidth; 0 means sizes stay unknown. */
  durationSeconds: number;
}

export interface ProbeOutcome {
  status: 'idle' | 'probing' | 'done' | 'error';
  container?: string;
  /** Frame size from the container, e.g. "640×360". */
  resolution?: string;
  rangeSupport?: string;
  size?: string;
  confidence?: string;
  message?: string;
  /** Selectable renditions, when the source is an HLS/DASH manifest. */
  qualities?: QualityOption[];
}

export interface NewIngestPageProps {
  value: IngestFormValue;
  onChange: (next: IngestFormValue) => void;
  probe: ProbeOutcome;
  probing: boolean;
  onProbe: () => void;
  onStart: () => void;
  /** Chosen rendition height, when the source advertises variants. */
  selectedQuality: number | null;
  onSelectQuality: (height: number) => void;
}

export function NewIngestPage({
  value,
  onChange,
  probe,
  probing,
  onProbe,
  onStart,
  selectedQuality,
  onSelectQuality,
}: NewIngestPageProps) {
  const set = (patch: Partial<IngestFormValue>) => onChange({ ...value, ...patch });

  return (
    <div className="grid-2">
      <Card title="Source">
        <div className="form">
          <div className="field field--full">
            <Field
              label="Source URL"
              value={value.url}
              onChange={(url) => set({ url })}
              hint="Direct file or manifest. Vyzor verifies the bytes before downloading."
            />
          </div>
          <Field
            label="Connections"
            type="number"
            min={1}
            max={32}
            value={String(value.connections)}
            onChange={(v) => set({ connections: Number(v) || 1 })}
          />
          <Field label="Save as" value={value.saveAs} onChange={(saveAs) => set({ saveAs })} />
          <Field
            label="Duration (seconds)"
            type="number"
            min={0}
            value={String(value.durationSeconds || '')}
            onChange={(v) => set({ durationSeconds: Math.max(0, Number(v) || 0) })}
            hint="Optional. Lets a manifest report real sizes per quality."
          />
          <div className="field field--full">
            <label>Pipeline steps</label>
            <SwitchRow
              name="Transcode to HLS"
              description="Adaptive bitrate ladder, never upscales"
              checked={value.hls}
              onChange={(hls) => set({ hls })}
            />
            <SwitchRow
              name="Upload to object storage"
              description="S3-compatible multipart upload"
              checked={value.upload}
              onChange={(upload) => set({ upload })}
            />
            <SwitchRow
              name="Purge CDN cache"
              description="Invalidate edge cache after upload"
              checked={value.purge}
              onChange={(purge) => set({ purge })}
            />
          </div>
        </div>
        <div className="form-actions">
          <Button variant="primary" onClick={onStart} disabled={!value.url}>
            Start download
          </Button>
          <Button loading={probing} onClick={onProbe} disabled={!value.url}>
            Probe only
          </Button>
        </div>
      </Card>

      <Card title="Probe result" subtitle="Dry run">
        <div className="kv">
          <div className="kv__row">
            <span className="kv__k">Status</span>
            <span className={probe.status === 'done' ? 'kv__v kv__v--strong' : 'kv__v muted'}>
              {probeLabel(probe)}
            </span>
          </div>
          <div className="kv__row">
            <span className="kv__k">Container</span>
            <span className="kv__v muted">{probe.container ?? '—'}</span>
          </div>
          <div className="kv__row">
            <span className="kv__k">Range support</span>
            <span className="kv__v muted">{probe.rangeSupport ?? '—'}</span>
          </div>
          <div className="kv__row">
            <span className="kv__k">Size</span>
            <span className="kv__v muted">{probe.size ?? '—'}</span>
          </div>
          {probe.confidence !== undefined && (
            <div className="kv__row">
              <span className="kv__k">Confidence</span>
              <span className="kv__v kv__v--strong">{probe.confidence}</span>
            </div>
          )}
        </div>
        {probe.status === 'done' && (
          <div style={{ marginTop: 18 }}>
            <label
              style={{
                display: 'block',
                fontSize: 13,
                fontWeight: 500,
                marginBottom: 8,
              }}
            >
              Download quality
            </label>
            <QualityPicker
              options={probe.qualities ?? []}
              selectedHeight={selectedQuality}
              onSelect={onSelectQuality}
            />
          </div>
        )}

        <Note className="note">
          Detection reads the raw bytes, never the file extension or Content-Type header.
          Sizes shown for a manifest come from its own declared bandwidth.
        </Note>
      </Card>
    </div>
  );
}

function probeLabel(probe: ProbeOutcome): string {
  switch (probe.status) {
    case 'probing':
      return 'Probing…';
    case 'done':
      return 'Video detected';
    case 'error':
      return probe.message ?? 'Probe failed';
    default:
      return 'Awaiting probe';
  }
}
