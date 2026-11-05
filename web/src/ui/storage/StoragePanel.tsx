/**
 * StoragePanel — the Settings view of the local-storage choice, plus what is
 * currently held.
 *
 * Settings needs to show the same chooser (so the decision can be revisited) and
 * the truth about what is stored: how much, and whether it will survive.
 */

import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { EmptyState } from '../primitives/EmptyState';
import { StorageChooser } from './StorageChooser';
import { VyzorStorageNotice, type VyzorStorageFacts } from './VyzorStorageNotice';
import { formatBytes } from '../../hooks/view-models';
import type { StorageEnvironment, StorageTierId } from '../../platform/storage-capabilities';
import type { StoredItem } from '../../platform/media-store';

export interface StoragePanelProps {
  environment: StorageEnvironment | null;
  detecting: boolean;
  selected: StorageTierId;
  directoryName: string | null;
  directoryPermission?: 'granted' | 'prompt' | 'denied' | 'unknown';
  error: string | null;
  items: ReadonlyArray<StoredItem>;
  usage: number;
  offlineStatus: string;
  /** Where Vyzor itself writes downloads, when that is known. */
  vyzorStorage?: VyzorStorageFacts | null;
  busy?: boolean;
  onChoose: (tier: StorageTierId, directory?: FileSystemDirectoryHandle) => void;
  onRegrant?: () => void;
  onRequestPersistence: () => void;
  onRefresh: () => void;
  onRemove: (id: string) => void;
  onPlay: (id: string) => void;
}

export function StoragePanel({
  environment,
  detecting,
  selected,
  directoryName,
  directoryPermission = 'unknown',
  error,
  items,
  usage,
  offlineStatus,
  vyzorStorage = null,
  busy = false,
  onChoose,
  onRegrant,
  onRequestPersistence,
  onRefresh,
  onRemove,
  onPlay,
}: StoragePanelProps) {
  // A remembered folder that the browser no longer lets us write through. This
  // is the common case after the app is rebuilt: the handle survives in
  // IndexedDB but the permission drops to "prompt", so the folder looks empty
  // until access is granted again. Saying so is the difference between "your
  // files are gone" and "one tap to see them".
  const needsRegrant =
    Boolean(directoryName) && directoryPermission !== 'granted' && Boolean(onRegrant);

  return (
    <>
      {needsRegrant && (
        <Card
          title="Your folder needs permission again"
          subtitle="The browser revoked access — the files are still on disk"
        >
          <p className="muted" style={{ marginBottom: 12 }}>
            <strong>{directoryName}</strong> was remembered, but this build has not been
            granted access yet, so its contents cannot be listed. Granting access reads the
            folder as it is — nothing is moved or rewritten.
          </p>
          <Button size="sm" variant="primary" onClick={onRegrant} loading={busy}>
            Grant access to {directoryName}
          </Button>
        </Card>
      )}

      {/* First, because it is the actual answer to "where do my downloads go?" —
          the browser options below are only a second copy. */}
      {vyzorStorage && (
        <Card
          title="Where downloads are written"
          subtitle="Vyzor owns this path, not the browser"
        >
          <VyzorStorageNotice
            facts={vyzorStorage}
            // A same-device Vyzor is writing to a real directory the user can
            // reach from another app; a remote one is not.
            browsable={vyzorStorage.sameDevice}
          />
        </Card>
      )}

      <Card title="Local storage" subtitle="Where downloaded media is kept on this device">
        {environment ? (
          <StorageChooser
            environment={environment}
            selected={selected}
            directoryName={directoryName}
            directoryPermission={directoryPermission}
            busy={busy || detecting}
            error={error}
            onChoose={onChoose}
            {...(onRegrant ? { onRegrant } : {})}
            onRequestPersistence={onRequestPersistence}
            onRefresh={onRefresh}
          />
        ) : (
          <p className="muted">{detecting ? 'Checking this device…' : 'Storage could not be read.'}</p>
        )}
      </Card>

      <Card
        title="Stored on this device"
        subtitle={
          items.length === 0
            ? 'Nothing stored yet'
            : `${items.length} file${items.length === 1 ? '' : 's'} · ${formatBytes(usage)}`
        }
        actions={
          <Button size="sm" variant="ghost" onClick={onRefresh} loading={busy || detecting}>
            Refresh
          </Button>
        }
        flush
      >
        {items.length === 0 ? (
          <EmptyState
            title="Nothing stored yet"
            description="Downloads saved to this device will be listed here."
          />
        ) : (
          <div className="flist" role="list">
            {items.map((item) => (
              <div className="frow" key={item.id} role="listitem">
                <span className="frow__thumb">
                  <span className="frow__glyph">
                    <span aria-hidden="true">●</span>
                  </span>
                </span>
                <span className="frow__main">
                  <span className="frow__name" title={item.name}>
                    {item.name}
                  </span>
                  <span className="frow__sub">
                    {formatBytes(item.size)} · {item.tier}
                    {item.locationHint ? ` · ${item.locationHint}` : ''}
                  </span>
                </span>
                <span className="frow__actions">
                  <Button size="sm" variant="ghost" onClick={() => onPlay(item.id)}>
                    Play
                  </Button>
                  <Button size="sm" variant="ghost" onClick={() => onRemove(item.id)}>
                    Remove
                  </Button>
                </span>
              </div>
            ))}
          </div>
        )}
      </Card>

      <Card title="Offline">
        <div className="kv">
          <div className="kv__row">
            <span className="kv__k">Service worker</span>
            <span className="kv__v">{offlineStatus}</span>
          </div>
          <div className="kv__row">
            <span className="kv__k">Installed app</span>
            <span className="kv__v">{environment?.standalone ? 'Yes' : 'No — running in a tab'}</span>
          </div>
          <div className="kv__row">
            <span className="kv__k">Storage protected from clearing</span>
            <span className="kv__v">{environment?.persisted ? 'Yes' : 'No'}</span>
          </div>
        </div>
      </Card>
    </>
  );
}
