/**
 * SettingsPage — tabbed: Connection, Storage location, Vyzor, More.
 *
 * "More" holds the diagnostics that are useful but not everyday: the pipeline
 * inspector for a chosen download. It lives here rather than on the Overview
 * because it is a per-item detail, not a dashboard summary.
 */

import { useMemo } from 'react';
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
  type VyzorLocationFacts,
} from '../settings/StorageLocationPanel';
import { EgressPanel, type EgressValue } from '../settings/EgressPanel';
import { Note } from '../primitives/Note';
import type { StorageCredentialsValue } from './StoragePage';
import type { DownloadView } from '../../hooks/view-models';

/** The Cloudflare delivery settings, split out so the tab owns only what it shows. */
export interface DeliveryCredentialsValue {
  cdnBase: string;
  cfZone: string;
  cfToken: string;
}

export type SettingsTab =
  | 'general'
  | 'location'
  | 'vyzor'
  | 'object'
  | 'cdn'
  | 'egress'
  | 'storage'
  | 'more';

export interface VyzorFacts {
  vyzorVersion: string;
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
  location: StorageLocationValue;
  onLocationChange: (next: StorageLocationValue) => void;
  /** Vyzor's real directory and volume, or null while loading. */
  vyzorLocation: VyzorLocationFacts | null;
  onSaveConnection: () => void;
  onSaveLocation: (dir: string) => void;
  /** The cookies and proxy Vyzor uses to reach a refusing host. */
  egress: EgressValue | null;
  /** Save cookies and/or proxy; a field set to null is left unchanged. */
  onSaveEgress: (next: { cookies: string | null; proxy: string | null }) => void;
  /** Clear the stored cookies. */
  onClearCookies: () => void;
  /** The local-storage section, rendered on its own tab. */
  storageSection?: React.ReactNode;
  /**
   * Object-storage credentials, on their own tab.
   *
   * Moved here from Storage: these are set once and then left alone, which is
   * the definition of a setting, not of something you look at while browsing
   * your files.
   */
  credentials: StorageCredentialsValue;
  onCredentialsChange: (next: StorageCredentialsValue) => void;
  onTestStorage: () => void;
  /** Cloudflare delivery credentials, on their own tab. */
  delivery: DeliveryCredentialsValue;
  onDeliveryChange: (next: DeliveryCredentialsValue) => void;
  /** Downloads selectable in the pipeline inspector ("More" tab). */
  downloads: ReadonlyArray<DownloadView>;
  /** Which download the inspector shows. */
  inspectedId: string | null;
  onInspectChange: (id: string) => void;
  /** Which action is in flight, so its button can spin. */
  busy?: string | null;
  usage?: { library: string; transferred: string; free: string; usedPercent: number; summary: string };
  vyzor: VyzorFacts;
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
          { id: 'vyzor', label: 'Vyzor' },
          { id: 'object', label: 'Object storage' },
          { id: 'cdn', label: 'CDN & delivery' },
          { id: 'egress', label: 'Egress' },
          { id: 'storage', label: 'Local storage' },
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
          </Card>
        </div>
      )}

      {tab === 'location' && (
        <StorageLocationPanel
          value={props.location}
          onChange={props.onLocationChange}
          vyzorLocation={props.vyzorLocation}
          onSave={props.onSaveLocation}
          {...(props.busy ? { busy: props.busy } : {})}
          {...(props.usage ? { usage: props.usage } : {})}
        />
      )}

      {tab === 'vyzor' && (
        <Card title="Vyzor capabilities">
          <div className="kv">
            <div className="kv__row">
              <span className="kv__k">Vyzor version</span>
              <span className="kv__v">{props.vyzor.vyzorVersion}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">FFmpeg</span>
              <span className="kv__v">{props.vyzor.ffmpeg}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">HLS transcoding</span>
              <span className="kv__v kv__v--strong">{props.vyzor.hls ? 'Enabled' : 'Disabled'}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">Object storage</span>
              <span className="kv__v kv__v--strong">
                {props.vyzor.upload ? 'Enabled' : 'Disabled'}
              </span>
            </div>
            <div className="kv__row">
              <span className="kv__k">CDN purge</span>
              <span className="kv__v kv__v--strong">{props.vyzor.purge ? 'Enabled' : 'Disabled'}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">Max request body</span>
              <span className="kv__v">{props.vyzor.maxBody}</span>
            </div>
            <div className="kv__row">
              <span className="kv__k">Worker threads</span>
              <span className="kv__v">{props.vyzor.workers}</span>
            </div>
          </div>
        </Card>
      )}

      {tab === 'object' && (
        <Card title="S3-compatible credentials">
          <div className="form">
            <Field
              label="Endpoint"
              value={props.credentials.endpoint}
              onChange={(endpoint) => props.onCredentialsChange({ ...props.credentials, endpoint })}
            />
            <Field
              label="Region"
              value={props.credentials.region}
              onChange={(region) => props.onCredentialsChange({ ...props.credentials, region })}
            />
            <Field
              label="Bucket"
              value={props.credentials.bucket}
              onChange={(bucket) => props.onCredentialsChange({ ...props.credentials, bucket })}
            />
            <SecretField
              label="Access key ID"
              value={props.credentials.accessKey}
              onChange={(accessKey) => props.onCredentialsChange({ ...props.credentials, accessKey })}
            />
            <SecretField
              label="Secret access key"
              value={props.credentials.secretKey}
              onChange={(secretKey) => props.onCredentialsChange({ ...props.credentials, secretKey })}
            />
          </div>
          <div className="form-actions">
            <Button
              variant="primary"
              loading={props.busy === 'test-storage'}
              onClick={props.onTestStorage}
            >
              Test connection
            </Button>
          </div>
        </Card>
      )}

      {tab === 'cdn' && (
        <Card title="CDN & delivery">
          <div className="form">
            <Field
              label="CDN base URL"
              value={props.delivery.cdnBase}
              onChange={(cdnBase) => props.onDeliveryChange({ ...props.delivery, cdnBase })}
              hint="Used to build public artifact URLs."
            />
            <Field
              label="Cloudflare zone ID"
              value={props.delivery.cfZone}
              onChange={(cfZone) => props.onDeliveryChange({ ...props.delivery, cfZone })}
            />
            <SecretField
              label="Cloudflare API token"
              value={props.delivery.cfToken}
              onChange={(cfToken) => props.onDeliveryChange({ ...props.delivery, cfToken })}
            />
          </div>
          <Note className="note">
            Credentials are held in memory for this session only. They are never written to
            browser storage and never persisted on the server.
          </Note>
        </Card>
      )}

      {tab === 'egress' && (
        <EgressPanel
          value={props.egress}
          {...(props.busy ? { saving: true } : {})}
          onSave={props.onSaveEgress}
          onClearCookies={props.onClearCookies}
        />
      )}

      {tab === 'storage' && props.storageSection}

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
 * PipelineInspector — pick a download and see how far each Vyzor stage got.
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
  // Newest first. The list arrives in creation order (oldest first), so this is
  // a reverse rather than a sort: `created_at` has one-second resolution, which
  // is not enough to order several downloads made in the same second — sorting
  // on it leaves ties and buries the one you just started.
  const ordered = useMemo(() => [...downloads].reverse(), [downloads]);
  const current = ordered.find((d) => d.id === inspectedId) ?? ordered[0] ?? null;

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
      {ordered.length > 1 && (
        <div className="field field--full" style={{ marginBottom: 14 }}>
          <span className="field__label">Download</span>
          <Select
            label="Download to inspect"
            value={current.id}
            options={ordered.map((d) => ({
              value: d.id,
              label: d.name,
              detail: d.state,
            }))}
            onChange={onInspectChange}
            searchable
            searchPlaceholder="Search downloads…"
          />
        </div>
      )}
      <PipelineSteps stages={current.pipeline} />
      <div className="field__hint" style={{ marginTop: 12 }}>
        Stages a download was not asked to run are shown as skipped. A stage the
        Vyzor does not report on stays pending rather than being assumed done.
      </div>
    </Card>
  );
}
