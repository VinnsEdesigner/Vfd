/**
 * StorageChooser — detect this device's storage options, then let the user pick.
 *
 * The point is honesty. A browser cannot write to "your filesystem" in the
 * general case: it can only write to a folder the user hands it, or to a
 * sandbox. So this lists what is genuinely available here, says plainly why
 * anything missing is missing, and states the limits of each option before the
 * choice is made rather than after something is lost.
 *
 * The folder tier needs a user gesture, so it cannot be selected silently — the
 * button that grants it is the choice.
 */

import { useState } from 'react';
import { Button } from '../primitives/Button';
import { Note } from '../primitives/Note';
import { IconFolder, IconDatabase, IconCheck, IconCloud } from '../primitives/Icons';
import { formatBytes } from '../../hooks/view-models';
import type { StorageEnvironment, StorageTier, StorageTierId } from '../../platform/storage-capabilities';

export interface StorageChooserProps {
  environment: StorageEnvironment;
  /** The tier currently in force. */
  selected: StorageTierId;
  /** Directory name already granted, if any. */
  directoryName: string | null;
  /** What the browser currently says about access to that directory. */
  directoryPermission?: 'granted' | 'prompt' | 'denied' | 'unknown';
  busy?: boolean;
  error?: string | null;
  onChoose: (tier: StorageTierId, directory?: FileSystemDirectoryHandle) => void;
  /** Re-grant a remembered folder. Must run from the click, not before it. */
  onRegrant?: () => void;
  onRequestPersistence: () => void;
  onRefresh: () => void;
}

function tierIcon(id: StorageTierId) {
  if (id === 'file-system-access') return <IconFolder />;
  if (id === 'opfs') return <IconDatabase />;
  return <IconCloud />;
}

/** The three or four facts that actually decide the choice. */
function traitsLine(tier: StorageTier): string {
  const t = tier.traits;
  const parts: string[] = [];
  parts.push(t.persistent ? 'Kept between visits' : 'This session only');
  parts.push(t.userVisible ? 'Visible in your file manager' : 'Inside the browser only');
  parts.push(t.largeFiles ? 'Fine for large files' : 'Best for small files');
  if (t.evictable) parts.push('The browser may clear it');
  return parts.join(' · ');
}

export function StorageChooser({
  environment,
  selected,
  directoryName,
  directoryPermission = 'unknown',
  busy = false,
  error = null,
  onChoose,
  onRegrant,
  onRequestPersistence,
  onRefresh,
}: StorageChooserProps) {
  const [granting, setGranting] = useState(false);
  const [grantError, setGrantError] = useState<string | null>(null);

  const pickFolder = async () => {
    setGrantError(null);
    if (typeof window.showDirectoryPicker !== 'function') {
      setGrantError('This browser has no folder picker.');
      return;
    }
    setGranting(true);
    try {
      const dir = await window.showDirectoryPicker({ id: 'vyzorix-media', mode: 'readwrite' });
      onChoose('file-system-access', dir);
    } catch (e) {
      /*
       * AbortError is the user closing the picker, which is a decision and not a
       * failure — saying nothing is right. But *every other* error must be shown:
       * swallowing them all is how a picker that never opens looks like a dead
       * button. A `SecurityError` (no user gesture), a `NotAllowedError` (a
       * policy refused it) and an unexpected throw each need to be visible.
       */
      if (e instanceof DOMException && e.name === 'AbortError') return;
      setGrantError(
        e instanceof Error && e.message
          ? e.message
          : 'The folder could not be opened.',
      );
    } finally {
      setGranting(false);
    }
  };

  const estimate = environment.estimate;

  return (
    <div className="storchooser">
      <div className="storchooser__head">
        <h2 className="storchooser__title">Where should downloads be kept?</h2>
        <p className="storchooser__sub">
          {environment.standalone
            ? 'Running as an installed app.'
            : 'Running in a browser tab.'}{' '}
          {environment.noneAvailable
            ? 'No local storage is available here, so downloads can only be streamed.'
            : 'This browser offers the options below.'}
        </p>
      </div>

      {/* When nothing can be stored, the reason is almost always the origin, and
          it is worth saying once at the top with the actual fix — rather than
          repeating it inside four cards. */}
      {environment.noneAvailable && !environment.secureContext && (
        <div className="storchooser__blocker">
          <strong>This page is not on a secure connection, so the browser blocks local storage.</strong>
          <p>
            Browsers only allow storage and offline support on HTTPS, or on{' '}
            <code>localhost</code> / <code>127.0.0.1</code>. A plain{' '}
            <code>http://&lt;ip-address&gt;</code> address — which is what you get
            reaching a self-hosted Vyzor from another device — is refused, so
            nothing can be saved on the device.
          </p>
          <p>
            To enable it, serve this page over HTTPS (a self-signed certificate is
            enough), or reach it through <code>localhost</code> on the same device.
          </p>
        </div>
      )}

      {estimate && (
        <div className="storchooser__usage">
          <span>
            {estimate.quota > 0
              ? `${formatBytes(estimate.usage)} of ${formatBytes(estimate.quota)} used by this app`
              : `${formatBytes(estimate.usage)} used by this app`}
          </span>
          {!environment.persisted && environment.canRequestPersistence && (
            <button type="button" className="storchooser__link" onClick={onRequestPersistence}>
              Ask the browser to keep it
            </button>
          )}
          {environment.persisted && <span className="storchooser__ok">Storage is protected</span>}
        </div>
      )}

      <ul className="storchooser__list">
        {environment.tiers.map((tier) => {
          const active = tier.id === selected;
          const disabled = !tier.available || busy || granting;
          return (
            <li key={tier.id}>
              <div
                className={
                  active
                    ? 'storchoice is-active'
                    : tier.available
                      ? 'storchoice'
                      : 'storchoice is-unavailable'
                }
              >
                <span className="storchoice__icon">{tierIcon(tier.id)}</span>
                <div className="storchoice__body">
                  <div className="storchoice__head">
                    <span className="storchoice__label">{tier.label}</span>
                    {tier.id === environment.recommended && tier.available && (
                      <span className="storchoice__badge">Recommended</span>
                    )}
                    {active && (
                      <span className="storchoice__chosen">
                        <IconCheck /> In use
                      </span>
                    )}
                  </div>
                  <p className="storchoice__summary">{tier.summary}</p>
                  <p className="storchoice__traits">{traitsLine(tier)}</p>
                  {tier.unavailableReason !== undefined && (
                    <p className="storchoice__reason">{tier.unavailableReason}</p>
                  )}
                  {tier.caveats.length > 0 && tier.available && (
                    <ul className="storchoice__caveats">
                      {tier.caveats.map((c) => (
                        <li key={c}>{c}</li>
                      ))}
                    </ul>
                  )}
                  {tier.id === 'file-system-access' && tier.available && (
                    <div className="storchoice__action">
                      {/* A remembered folder that needs re-confirming gets one
                          tap, not the whole picker again. */}
                      {directoryName && directoryPermission !== 'granted' && onRegrant ? (
                        <Button
                          size="sm"
                          variant="primary"
                          disabled={busy}
                          onClick={onRegrant}
                        >
                          <IconFolder />
                          Re-allow access to {directoryName}
                        </Button>
                      ) : (
                        <Button
                          size="sm"
                          variant={active ? 'default' : 'primary'}
                          loading={granting}
                          disabled={busy}
                          onClick={() => void pickFolder()}
                        >
                          <IconFolder />
                          {directoryName ? `Change folder (${directoryName})` : 'Choose a folder'}
                        </Button>
                      )}
                      {directoryName && directoryPermission !== 'granted' && (
                        <p className="storchoice__note">
                          This folder is remembered. The browser needs you to confirm access
                          again after a restart.
                        </p>
                      )}
                    </div>
                  )}
                  {tier.id === 'opfs' && tier.available && (
                    <div className="storchoice__action">
                      <Button
                        size="sm"
                        variant={active ? 'default' : 'primary'}
                        disabled={disabled}
                        onClick={() => onChoose('opfs')}
                      >
                        Use private storage
                      </Button>
                    </div>
                  )}
                  {tier.id === 'indexeddb' && tier.available && (
                    <div className="storchoice__action">
                      <Button
                        size="sm"
                        variant={active ? 'default' : 'default'}
                        disabled={disabled}
                        onClick={() => onChoose('indexeddb')}
                      >
                        Use the browser database
                      </Button>
                    </div>
                  )}
                </div>
              </div>
            </li>
          );
        })}
      </ul>

      {(error ?? grantError) !== null && (
        <Note className="note note--warn">{error ?? grantError}</Note>
      )}

      <div className="storchooser__foot">
        <span className="storchooser__hint">
          You can change this later in Settings.
        </span>
        <Button size="sm" variant="ghost" onClick={onRefresh} loading={busy}>
          Check again
        </Button>
      </div>
    </div>
  );
}
