/**
 * StoragePage — the downloaded files.
 *
 * The Files tab is the download library: in-progress cards on top (each with a
 * bar carrying % and transfer rate), then the file browser (grid or rows, with
 * its own filter/sort controls and multi-select).
 *
 * Object storage and CDN credentials used to live here as sibling tabs. They are
 * configuration rather than content — you set them once, where the connection
 * settings are, not every time you look at your files — so they moved to
 * Settings. The credentials type stays here because the fields are still defined
 * alongside the storage this page owns.
 */

import { useState } from 'react';
import { Card } from '../primitives/Card';
import { Button } from '../primitives/Button';
import { DownloadCardList } from '../downloads/DownloadCardList';
import { FileLibrary, type StorageTarget } from '../files';
import { IconShare } from '../primitives/Icons';
import type { DownloadView } from '../../hooks/view-models';

export type StorageTab = 'files';

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
  onSelect: (id: string) => void;
  onOpenFile: (id: string) => void;
  onGoToSettings: () => void;
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
  /** Save a Vyzor item onto this device. */
  onSaveLocally?: (id: string) => void;
  /** Ids already stored on this device. */
  storedIds?: ReadonlySet<string>;
  savingIds?: ReadonlySet<string>;
  /** Absolute URL of an item's bytes, for sharing and thumbnails. */
  contentUrl: (id: string) => string;
  /** Which action is in flight, so its button can spin. */
  busy?: string | null;
}

export function StoragePage(props: StoragePageProps) {
  const {
    downloads,
    ongoing,
    target,
    localPath,
    cloudPath,
    onSelect,
    onOpenFile,
    onGoToSettings,
    onCancelDownload,
    cancellingId = null,
    onDeleteFile,
    onDeleteFiles,
    onShareFiles,
    onMoveFiles,
    onRenameFile,
    onSaveLocally,
    storedIds,
    savingIds,
    contentUrl,
    busy = null,
  } = props;

  const doneCount = downloads.filter((d) => d.state === 'done').length;

  const deleteSelected = (ids: readonly string[]) => {
    if (onDeleteFiles) onDeleteFiles(ids);
    else ids.forEach((id) => onDeleteFile?.(id));
  };

  return (
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
          {...(onSaveLocally ? { onSaveLocally } : {})}
          {...(storedIds ? { storedIds } : {})}
          {...(savingIds ? { savingIds } : {})}
          busy={busy === 'file-action'}
        />
      </div>
    </Card>
  );
}

/** Exported so the page can own its tab state when used standalone. */
export function useStorageTab(initial: StorageTab = 'files') {
  return useState<StorageTab>(initial);
}
