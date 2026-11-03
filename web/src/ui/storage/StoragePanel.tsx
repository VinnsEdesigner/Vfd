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
import { formatBytes } from '../../hooks/view-models';
import type { StorageEnvironment, StorageTierId } from '../../platform/storage-capabilities';
import type { StoredItem } from '../../platform/media-store';

export interface StoragePanelProps {
  environment: StorageEnvironment | null;
  detecting: boolean;
  selected: StorageTierId;
  directoryName: string | null;
  error: string | null;
  items: ReadonlyArray<StoredItem>;
  usage: number;
  offlineStatus: string;
  busy?: boolean;
  onChoose: (tier: StorageTierId, directory?: FileSystemDirectoryHandle) => void;
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
  error,
  items,
  usage,
  offlineStatus,
  busy = false,
  onChoose,
  onRequestPersistence,
  onRefresh,
  onRemove,
  onPlay,
}: StoragePanelProps) {
  return (
    <>
      <Card title="Local storage" subtitle="Where downloaded media is kept on this device">
        {environment ? (
          <StorageChooser
            environment={environment}
            selected={selected}
            directoryName={directoryName}
            busy={busy || detecting}
            error={error}
            onChoose={onChoose}
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
