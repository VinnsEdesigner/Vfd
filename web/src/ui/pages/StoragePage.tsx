/**
 * StoragePage — tabbed: downloaded Files, Object storage, CDN & delivery.
 *
 * The Files tab is the download library: in-progress cards on top (each with a
 * bar carrying % and transfer rate), then the file browser (grid or rows, with
 * its own filter/sort controls and multi-select).
 */

import { useState } from 'react';
import { Card } from '../primitives/Card';
import { Tabs } from '../primitives/Tabs';
import { Button } from '../primitives/Button';
import { Field, SecretField } from '../primitives/Field';
import { Note } from '../primitives/Note';
import { DownloadCardList } from '../downloads/DownloadCardList';
import { FileLibrary, type StorageTarget } from '../files';
import { IconShare } from '../primitives/Icons';
import type { DownloadView } from '../../hooks/view-models';

export type StorageTab = 'files' | 'object' | 'cdn';

export interface StorageCredentialsValue {
  endpoint: string;
  region: string;
  bucket: string;
  accessKey: string;
  secretKey: string;
  cdnBase: string;
  cfZone: string;
  cfToken: string;
}

export interface StoragePageProps {
  tab: StorageTab;
  onTabChange: (tab: StorageTab) => void;
  downloads: ReadonlyArray<DownloadView>;
  ongoing: ReadonlyArray<DownloadView>;
  target: StorageTarget;
  localPath: string;
  cloudPath: string;
  credentials: StorageCredentialsValue;
  onCredentialsChange: (next: StorageCredentialsValue) => void;
  onSelect: (id: string) => void;
  onOpenFile: (id: string) => void;
  onGoToSettings: () => void;
  onTestStorage: () => void;
  onCancelDownload?: (id: string) => void;
  cancellingId?: string | null;
  /** Destructive; the parent confirms before calling. */
  onDeleteFile?: (id: string) => void;
  /** Destructive, in bulk; the parent confirms once before calling. */
  onDeleteFiles?: (ids: readonly string[]) => void;
  /** Outbound share; the parent owns the Web Share call and its toast. */
  onShareFiles?: (files: readonly DownloadView[]) => void;
  /** Relocate items into a library folder. */
  onMoveFiles?: (ids: readonly string[], folder: string) => void;
  /** Rename an item in place. */
  onRenameFile?: (id: string, name: string) => void;
  /** Absolute URL of an item's bytes, for sharing and thumbnails. */
  contentUrl: (id: string) => string;
  /** Which action is in flight, so its button can spin. */
  busy?: string | null;
}

export function StoragePage(props: StoragePageProps) {
  const {
    tab,
    onTabChange,
    downloads,
    ongoing,
    target,
    localPath,
    cloudPath,
    credentials,
    onCredentialsChange,
    onSelect,
    onOpenFile,
    onGoToSettings,
    onTestStorage,
    onCancelDownload,
    cancellingId = null,
    onDeleteFile,
    onDeleteFiles,
    onShareFiles,
    onMoveFiles,
    onRenameFile,
    contentUrl,
    busy = null,
  } = props;

  const doneCount = downloads.filter((d) => d.state === 'done').length;
  const set = (patch: Partial<StorageCredentialsValue>) =>
    onCredentialsChange({ ...credentials, ...patch });

  const deleteSelected = (ids: readonly string[]) => {
    if (onDeleteFiles) onDeleteFiles(ids);
    else ids.forEach((id) => onDeleteFile?.(id));
  };

  return (
    <>
      <Tabs
        label="Storage sections"
        active={tab}
        onChange={onTabChange}
        items={[
          { id: 'files', label: 'Files', count: doneCount },
          { id: 'object', label: 'Object storage' },
          { id: 'cdn', label: 'CDN & delivery' },
        ]}
      />

      {tab === 'files' && (
        <Card
          title="Downloaded files"
          subtitle={`${doneCount} finished · saving to ${
            target === 'local' ? 'this device' : 'object storage'
          }`}
          actions={
            <>
              <Button size="sm" variant="ghost" onClick={onGoToSettings}>
                Change location
              </Button>
              {onShareFiles && downloads.length > 0 && (
                <Button
                  size="sm"
                  onClick={() => onShareFiles(downloads.filter((d) => d.state === 'done'))}
                >
                  <IconShare /> Share
                </Button>
              )}
            </>
          }
          flush
        >
          <div style={{ padding: '20px 20px 0' }}>
            <div className="files__label">
              In progress{ongoing.length > 0 ? ` · ${ongoing.length}` : ''}
            </div>
            <DownloadCardList
              downloads={ongoing}
              onSelect={onSelect}
              {...(onCancelDownload ? { onCancel: onCancelDownload } : {})}
              cancellingId={cancellingId}
            />
          </div>
          <div className="files__library">
            <FileLibrary
              files={downloads}
              target={target}
              localPath={localPath}
              cloudPath={cloudPath}
              contentUrl={contentUrl}
              onOpen={onOpenFile}
              onShare={(list) => onShareFiles?.(list)}
              onDelete={deleteSelected}
              onMove={(ids, folder) => onMoveFiles?.(ids, folder)}
              {...(onRenameFile ? { onRename: onRenameFile } : {})}
              busy={busy === 'file-action'}
            />
          </div>
        </Card>
      )}

      {tab === 'object' && (
        <Card title="S3-compatible credentials">
          <div className="form">
            <Field
              label="Endpoint"
              value={credentials.endpoint}
              onChange={(endpoint) => set({ endpoint })}
            />
            <Field label="Region" value={credentials.region} onChange={(region) => set({ region })} />
            <Field label="Bucket" value={credentials.bucket} onChange={(bucket) => set({ bucket })} />
            <SecretField
              label="Access key ID"
              value={credentials.accessKey}
              onChange={(accessKey) => set({ accessKey })}
            />
            <SecretField
              label="Secret access key"
              value={credentials.secretKey}
              onChange={(secretKey) => set({ secretKey })}
            />
          </div>
          <div className="form-actions">
            <Button
              variant="primary"
              loading={busy === 'test-storage'}
              onClick={onTestStorage}
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
              value={credentials.cdnBase}
              onChange={(cdnBase) => set({ cdnBase })}
              hint="Used to build public artifact URLs."
            />
            <Field label="Cloudflare zone ID" value={credentials.cfZone} onChange={(cfZone) => set({ cfZone })} />
            <SecretField
              label="Cloudflare API token"
              value={credentials.cfToken}
              onChange={(cfToken) => set({ cfToken })}
            />
          </div>
          <Note className="note">
            Credentials are held in memory for this session only. They are never written to
            browser storage and never persisted on the server.
          </Note>
        </Card>
      )}
    </>
  );
}

/** Exported so the page can own its tab state when used standalone. */
export function useStorageTab(initial: StorageTab = 'files') {
  return useState<StorageTab>(initial);
}
