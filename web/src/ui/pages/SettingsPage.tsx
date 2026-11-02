/**
 * SettingsPage — tabbed: Connection, Storage location, Engine.
 */

import { Card } from '../primitives/Card';
import { Tabs } from '../primitives/Tabs';
import { Button } from '../primitives/Button';
import { Field, SecretField } from '../primitives/Field';
import { SwitchRow } from '../primitives/SwitchRow';
import {
  StorageLocationPanel,
  type StorageLocationValue,
} from '../settings/StorageLocationPanel';

export type SettingsTab = 'general' | 'location' | 'engine';

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
    </>
  );
}
