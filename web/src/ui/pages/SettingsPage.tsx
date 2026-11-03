/**
 * SettingsPage — tabbed: Connection, Storage location, Engine, More.
 *
 * "More" holds the diagnostics that are useful but not everyday: the pipeline
 * inspector for a chosen download. It lives here rather than on the Overview
 * because it is a per-item detail, not a dashboard summary.
 */

import { Card } from '../primitives/Card';
import { Tabs } from '../primitives/Tabs';
import { Button } from '../primitives/Button';
import { Field, SecretField } from '../primitives/Field';
import { SwitchRow } from '../primitives/SwitchRow';
import { PipelineSteps } from '../downloads/PipelineSteps';
import { StatusBadge } from '../downloads/StatusBadge';
import { EmptyState } from '../primitives/EmptyState';
import { Select } from '../primitives/Select';
import {
  StorageLocationPanel,
  type StorageLocationValue,
} from '../settings/StorageLocationPanel';
import type { DownloadView } from '../../hooks/view-models';

export type SettingsTab = 'general' | 'location' | 'engine' | 'more';

export interface EngineFacts {
  engineVersion: string;
  ffmpeg: string;
  hls: boolean;
  upload: boolean;
  purge: boolean;
  maxBody: string;
  workers: string;
}

export interface SettingsPageProps {
  tab: SettingsTab;
  onTabChange: (tab: SettingsTab) => void;
  endpoint: string;
  onEndpointChange: (v: string) => void;
  authToken: string;
  onAuthTokenChange: (v: string) => void;
  darkTheme: boolean;
  onDarkThemeChange: (v: boolean) => void;
  autoStart: boolean;
  onAutoStartChange: (v: boolean) => void;
  verifyChecksums: boolean;
  onVerifyChecksumsChange: (v: boolean) => void;
  location: StorageLocationValue;
  onLocationChange: (next: StorageLocationValue) => void;
  onSaveConnection: () => void;
  onSaveLocation: () => void;
  onTestLocation: () => void;
  /** Downloads selectable in the pipeline inspector ("More" tab). */
  downloads: ReadonlyArray<DownloadView>;
  /** Which download the inspector shows. */
  inspectedId: string | null;
  onInspectChange: (id: string) => void;
  /** Which action is in flight, so its button can spin. */
  busy?: string | null;
  usage?: { library: string; transferred: string; free: string; usedPercent: number; summary: string };
  engine: EngineFacts;
}

export function SettingsPage(props: SettingsPageProps) {
  const { tab, onTabChange } = props;

  return (
    <>
      <Tabs
        label="Settings sections"
        active={tab}
        onChange={onTabChange}
        items={[
          { id: 'general', label: 'Connection' },
          { id: 'location', label: 'Storage location' },
          { id: 'engine', label: 'Engine' },
          { id: 'more', label: 'More' },
        ]}
      />

      {tab === 'general' && (
        <div className="grid-2 grid-2--even">
          <Card title="API connection">
            <div className="form">
              <div className="field field--full">
                <Field
                  label="API endpoint"
                  value={props.endpoint}
                  onChange={props.onEndpointChange}
                />
              </div>
              <div className="field field--full">
                <SecretField
                  label="Bearer token"
                  value={props.authToken}
                  onChange={props.onAuthTokenChange}
                  hint="Required for all endpoints except health checks."
                />
              </div>
            </div>
            <div className="form-actions">
              <Button
                variant="primary"
                loading={props.busy === 'save-connection'}
                onClick={props.onSaveConnection}
              >
                Save &amp; reconnect
              </Button>
            </div>
          </Card>

          <Card title="Behaviour">
            <SwitchRow
              name="Dark theme"
              description="Follows your last choice in the header"
              checked={props.darkTheme}
              onChange={props.onDarkThemeChange}
            />
            <SwitchRow
              name="Start downloads automatically"
              description="Begin fetching as soon as a source is added"
              checked={props.autoStart}
              onChange={props.onAutoStartChange}
            />
            <SwitchRow
              name="Verify checksums"
              description="Hash every finished file with SHA-256"
              checked={props.verifyChecksums}
              onChange={props.onVerifyChecksumsChange}
            />
          </Card>
        </div>
      )}

      {tab === 'location' && (
        <StorageLocationPanel
          value={props.location}
          onChange={props.onLocationChange}
          onSave={props.onSaveLocation}
          onTest={props.onTestLocation}
          {...(props.busy ? { busy: props.busy } : {})}
          {...(props.usage ? { usage: props.usage } : {})}
        />
      )}

      {tab === 'engine' && (
        <Card title="Engine capabilities">
          <div className="kv">
            <div className="kv__row">
              <span className="kv__k">Engine version</span>
              <span className="kv__v">{props.engine.engineVersion}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">FFmpeg</span>
              <span className="kv__v">{props.engine.ffmpeg}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">HLS transcoding</span>
              <span className="kv__v kv__v--strong">{props.engine.hls ? 'Enabled' : 'Disabled'}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">Object storage</span>
              <span className="kv__v kv__v--strong">
                {props.engine.upload ? 'Enabled' : 'Disabled'}
              </span>
            </div>
            <div className="kv__row">
              <span className="kv__k">CDN purge</span>
              <span className="kv__v kv__v--strong">{props.engine.purge ? 'Enabled' : 'Disabled'}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">Max request body</span>
              <span className="kv__v">{props.engine.maxBody}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">Worker threads</span>
              <span className="kv__v">{props.engine.workers}</span>
            </div>
          </div>
        </Card>
      )}

      {tab === 'more' && (
        <PipelineInspector
          downloads={props.downloads}
          inspectedId={props.inspectedId}
          onInspectChange={props.onInspectChange}
        />
      )}
    </>
  );
}

/**
 * PipelineInspector — pick a download and see how far each engine stage got.
 *
 * The stage statuses come from the record's own facts (see `pipelineStages`),
 * so this shows what actually happened rather than a fixed diagram.
 */
function PipelineInspector({
  downloads,
  inspectedId,
  onInspectChange,
}: {
  downloads: ReadonlyArray<DownloadView>;
  inspectedId: string | null;
  onInspectChange: (id: string) => void;
}) {
  const current = downloads.find((d) => d.id === inspectedId) ?? downloads[0] ?? null;

  if (!current) {
    return (
      <Card title="Pipeline inspector">
        <EmptyState
          title="Nothing to inspect"
          description="Start a download and its pipeline will appear here."
        />
      </Card>
    );
  }

  return (
    <Card
      title="Pipeline inspector"
      subtitle={current.name}
      actions={<StatusBadge state={current.state} />}
    >
      {downloads.length > 1 && (
        <div className="field field--full" style={{ marginBottom: 14 }}>
          <span className="field__label">Download</span>
          <Select
            label="Download to inspect"
            value={current.id}
            options={downloads.map((d) => ({
              value: d.id,
              label: d.name,
              detail: d.state,
            }))}
            onChange={onInspectChange}
          />
        </div>
      )}
      <PipelineSteps stages={current.pipeline} />
      <div className="field__hint" style={{ marginTop: 12 }}>
        Stages a download was not asked to run are shown as skipped. A stage the
        engine does not report on stays pending rather than being assumed done.
      </div>
    </Card>
  );
}
