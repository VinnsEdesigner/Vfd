/**
 * Saving a finished download onto this device, and playing it back.
 *
 * The bytes are streamed straight from the engine into the chosen store, so a
 * large file never has to sit in memory: the response body is read chunk by
 * chunk and each chunk is written as it arrives. That is what makes the
 * folder and private-storage tiers usable for real media, and it is why the
 * IndexedDB tier is flagged as small-file-only — it buffers.
 *
 * Nothing here claims success it cannot prove: the size written is checked
 * against the size the engine reported, and a mismatch fails the save.
 */

import { useCallback, useEffect, useMemo, useState } from 'react';
import { createMediaStore, type MediaStore, type StoredItem } from '../platform/media-store';
import { cacheMediaOffline } from '../platform/service-worker';
import { useLocalStorageStore } from '../stores/local-storage-store';
import { useMediaApi } from './provider';
import type { DownloadView } from './view-models';

export interface LocalMediaState {
  /** What is currently on this device. */
  items: ReadonlyArray<StoredItem>;
  /** Bytes held locally. */
  usage: number;
  /** Ids being saved right now. */
  saving: ReadonlySet<string>;
  /** 0..1 per id while saving. */
  progress: ReadonlyMap<string, number>;
  /**
   * A playable URL per stored file, resolved as the list loads so the file
   * manager can show and play a device file without a second, click-time fetch.
   * A blob URL for the object tiers; the sandbox path for a real folder.
   */
  urls: ReadonlyMap<string, string>;
  loading: boolean;
  error: string | null;
  refresh: () => Promise<void>;
  /** Streams a finished download into local storage. */
  save: (file: DownloadView) => Promise<StoredItem>;
  /** A playable URL, or null when the file is not on this device. */
  urlFor: (id: string) => Promise<string | null>;
  /**
   * Rename a stored file. For the folder tier this renames the file on disk, so
   * the change is real outside the app too. Returns the new id, which for that
   * tier is the new filename.
   */
  rename: (id: string, name: string) => Promise<StoredItem | null>;
  remove: (id: string) => Promise<void>;
  has: (id: string) => boolean;
}

export function useLocalMedia(): LocalMediaState {
  const { api } = useMediaApi();
  const tier = useLocalStorageStore((s) => s.effectiveTier());
  const directory = useLocalStorageStore((s) => s.directory);

  const [items, setItems] = useState<ReadonlyArray<StoredItem>>([]);
  const [usage, setUsage] = useState(0);
  const [saving, setSaving] = useState<ReadonlySet<string>>(new Set());
  const [progress, setProgress] = useState<ReadonlyMap<string, number>>(new Map());
  const [urls, setUrls] = useState<ReadonlyMap<string, string>>(new Map());
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  /** The store for the current choice. Rebuilt when the choice changes. */
  const store = useMemo<Promise<MediaStore>>(
    () =>
      createMediaStore({
        tier,
        ...(directory ? { directory } : {}),
      }),
    [tier, directory],
  );

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const s = await store;
      const [list, used] = await Promise.all([s.list(), s.usage()]);
      setItems(list);
      setUsage(used);
      setError(null);
      // Resolve a playable URL per file up front, so the library can render and
      // play a device file directly. A failure for one file must not sink the
      // list: it simply has no URL.
      const next = new Map<string, string>();
      await Promise.all(
        list.map(async (item) => {
          try {
            const url = await s.urlFor(item.id);
            if (url) next.set(item.id, url);
          } catch {
            /* unreadable file: leave it without a URL */
          }
        }),
      );
      setUrls((prev) => {
        // Release URLs for files that are gone, or a long session leaks blobs.
        for (const [id, url] of prev) {
          if (!next.has(id)) s.release(url);
        }
        return next;
      });
    } catch (e) {
      // A tier that cannot be opened is reported, never hidden behind an empty
      // list, because an empty list looks like "nothing saved" and would be a lie.
      setError(e instanceof Error ? e.message : 'Local storage could not be read.');
      setItems([]);
      setUsage(0);
      setUrls(new Map());
    } finally {
      setLoading(false);
    }
  }, [store]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const save = useCallback(
    async (file: DownloadView): Promise<StoredItem> => {
      const s = await store;
      setSaving((prev) => new Set(prev).add(file.id));
      setProgress((prev) => new Map(prev).set(file.id, 0));

      const url = api.client.transport.resolve(
        `/api/v1/downloads/${encodeURIComponent(file.id)}/content`,
      );
      let writer: Awaited<ReturnType<MediaStore['createWriter']>> | null = null;
      try {
        const response = await fetch(url);
        if (!response.ok) throw new Error(`the engine returned HTTP ${response.status}`);
        if (!response.body) throw new Error('the engine sent no body to stream');

        writer = await s.createWriter({ id: file.id, name: file.filename, type: mimeOf(file) });

        const total = Number(response.headers.get('Content-Length') ?? 0) || file.bytes;
        const reader = response.body.getReader();
        let written = 0;
        for (;;) {
          const { done, value } = await reader.read();
          if (done) break;
          if (!value) continue;
          await writer.write(value);
          written += value.byteLength;
          if (total > 0) {
            setProgress((prev) => new Map(prev).set(file.id, Math.min(written / total, 1)));
          }
        }
        await writer.close(written);
        writer = null;

        // A short write is a failed save, not a success with a small file.
        if (total > 0 && written !== total) {
          await s.remove(file.id);
          throw new Error(`wrote ${written} of ${total} bytes`);
        }

        const item = (await s.get(file.id)) ?? {
          id: file.id,
          name: file.filename,
          type: mimeOf(file),
          size: written,
          tier,
        };
        await refresh();
        // Best-effort: also hand it to the service worker so it survives offline.
        void cacheMediaOffline(url);
        return item;
      } catch (e) {
        if (writer) await writer.abort().catch(() => undefined);
        throw e;
      } finally {
        setSaving((prev) => {
          const next = new Set(prev);
          next.delete(file.id);
          return next;
        });
        setProgress((prev) => {
          const next = new Map(prev);
          next.delete(file.id);
          return next;
        });
      }
    },
    [store, api.client.transport, tier, refresh],
  );

  const urlFor = useCallback(
    async (id: string) => {
      const s = await store;
      return s.urlFor(id);
    },
    [store],
  );

  const rename = useCallback(
    async (id: string, name: string) => {
      const s = await store;
      const result = await s.rename(id, name);
      await refresh();
      return result;
    },
    [store, refresh],
  );

  const remove = useCallback(
    async (id: string) => {
      const s = await store;
      await s.remove(id);
      await refresh();
    },
    [store, refresh],
  );

  const present = useMemo(() => new Set(items.map((i) => i.id)), [items]);
  const has = useCallback((id: string) => present.has(id), [present]);

  return {
    items,
    usage,
    saving,
    progress,
    urls,
    loading,
    error,
    refresh,
    save,
    urlFor,
    rename,
    remove,
    has,
  };
}

/** The MIME type to store, from the media the engine detected. */
function mimeOf(file: DownloadView): string {
  if (file.isImage) {
    const ext = file.extension;
    if (ext === 'png') return 'image/png';
    if (ext === 'gif') return 'image/gif';
    if (ext === 'webp') return 'image/webp';
    if (ext === 'bmp') return 'image/bmp';
    return 'image/jpeg';
  }
  if (file.isAudio) {
    if (file.extension === 'mp3') return 'audio/mpeg';
    if (file.extension === 'wav') return 'audio/wav';
    if (file.extension === 'flac') return 'audio/flac';
    return 'audio/mp4';
  }
  if (file.extension === 'webm') return 'video/webm';
  if (file.extension === 'ogv') return 'video/ogg';
  return 'video/mp4';
}
