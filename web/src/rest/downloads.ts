/**
 * Downloads resource. Sits between the transport and the hooks: it knows the URL
 * shapes and the wire request bodies, and returns normalised domain objects.
 */

import { storageHeaders, type ApiClient } from '../api-client';
import { normalizeDownload, normalizeDownloadList } from '../domain/normalize';
import type { CreateDownloadInput, Download, StorageCredentials } from '../domain/media';
import type {
  WireCreateDownloadRequest,
  WireDeleteResult,
  WireDownload,
  WireDownloadList,
  WireMoveRequest,
  WireRenameRequest,
  WireActionRequest,
} from '../domain/wire';

export interface DownloadsResource {
  list(signal?: AbortSignal): Promise<Download[]>;
  get(id: string, signal?: AbortSignal): Promise<Download>;
  create(input: CreateDownloadInput, creds?: StorageCredentials, signal?: AbortSignal): Promise<Download>;
  /** Cancel stops the transfer but keeps the record. */
  cancel(id: string, signal?: AbortSignal): Promise<void>;
  /**
   * Delete removes the record, and with `removeFiles` also deletes the
   * downloaded artifacts from disk. Irreversible, so it is opt-in.
   */
  remove(
    id: string,
    removeFiles: boolean,
    signal?: AbortSignal,
  ): Promise<{ filesRemoved: boolean }>;
  /** Rename an item. The bytes stay put; only the name changes. */
  rename(id: string, name: string, signal?: AbortSignal): Promise<Download>;
  /** Move an item into a named folder inside the library. */
  move(id: string, folder: string, signal?: AbortSignal): Promise<Download>;
  /** Stop a running transfer, keeping the partial data for a resume. */
  pause(id: string, signal?: AbortSignal): Promise<Download>;
  /** Continue a paused transfer from where it stopped. */
  resume(id: string, signal?: AbortSignal): Promise<Download>;
  /**
   * Convert a container the browser cannot play (MKV, AVI) into MP4. Runs in
   * Vyzor's background; the returned record reports `conversion.state`,
   * which the caller polls until it settles.
   */
  convert(id: string, signal?: AbortSignal): Promise<Download>;
}

function toWireRequest(input: CreateDownloadInput): WireCreateDownloadRequest {
  const req: WireCreateDownloadRequest = {
    url: input.url,
    hls: input.hls,
    upload: input.upload,
    purge: input.purge,
    connections: input.connections,
  };
  if (input.allowNonMedia) req.allow_non_media = true;
  if (input.quality) req.quality = input.quality;
  if (input.prefix) req.prefix = input.prefix;
  if (input.hlsDir) req.hls_dir = input.hlsDir;
  // A `.torrent` file, or a file selection within a torrent. Either way the
  // server turns it into a magnet the client can fetch.
  if (input.torrentFile) req.torrent_file = input.torrentFile;
  if (input.magnetFiles && input.magnetFiles.length > 0) req.magnet_files = input.magnetFiles;
  return req;
}

export function createDownloadsResource(client: ApiClient): DownloadsResource {
  return {
    async list(signal) {
      const wire = await client.transport.request<WireDownloadList>({
        method: 'GET',
        path: '/api/v1/downloads',
        ...(signal ? { signal } : {}),
      });
      return normalizeDownloadList(wire);
    },

    async get(id, signal) {
      const wire = await client.transport.request<WireDownload>({
        method: 'GET',
        path: `/api/v1/downloads/${encodeURIComponent(id)}`,
        ...(signal ? { signal } : {}),
      });
      return normalizeDownload(wire);
    },

    async create(input, creds, signal) {
      const wire = await client.transport.request<WireDownload>({
        method: 'POST',
        path: '/api/v1/downloads',
        body: toWireRequest(input),
        headers: storageHeaders(creds),
        ...(signal ? { signal } : {}),
      });
      return normalizeDownload(wire);
    },

    async cancel(id, signal) {
      await client.transport.request<void>({
        method: 'DELETE',
        path: `/api/v1/downloads/${encodeURIComponent(id)}`,
        ...(signal ? { signal } : {}),
      });
    },

    async remove(id, removeFiles, signal) {
      const wire = await client.transport.request<WireDeleteResult>({
        method: 'DELETE',
        path: `/api/v1/downloads/${encodeURIComponent(id)}`,
        query: { files: removeFiles ? 1 : 0 },
        ...(signal ? { signal } : {}),
      });
      return { filesRemoved: Boolean(wire?.files_removed) };
    },

    async rename(id, name, signal) {
      const wire = await client.transport.request<WireDownload>({
        method: 'PATCH',
        path: `/api/v1/downloads/${encodeURIComponent(id)}`,
        body: { name } satisfies WireRenameRequest,
        ...(signal ? { signal } : {}),
      });
      return normalizeDownload(wire);
    },

    async move(id, folder, signal) {
      const wire = await client.transport.request<WireDownload>({
        method: 'PATCH',
        path: `/api/v1/downloads/${encodeURIComponent(id)}`,
        body: { move_to: folder } satisfies WireMoveRequest,
        ...(signal ? { signal } : {}),
      });
      return normalizeDownload(wire);
    },

    async pause(id, signal) {
      const wire = await client.transport.request<WireDownload>({
        method: 'PATCH',
        path: `/api/v1/downloads/${encodeURIComponent(id)}`,
        body: { action: 'pause' } satisfies WireActionRequest,
        ...(signal ? { signal } : {}),
      });
      return normalizeDownload(wire);
    },

    async resume(id, signal) {
      const wire = await client.transport.request<WireDownload>({
        method: 'PATCH',
        path: `/api/v1/downloads/${encodeURIComponent(id)}`,
        body: { action: 'resume' } satisfies WireActionRequest,
        ...(signal ? { signal } : {}),
      });
      return normalizeDownload(wire);
    },

    async convert(id, signal) {
      const wire = await client.transport.request<WireDownload>({
        method: 'PATCH',
        path: `/api/v1/downloads/${encodeURIComponent(id)}`,
        body: { action: 'convert' } satisfies WireActionRequest,
        ...(signal ? { signal } : {}),
      });
      return normalizeDownload(wire);
    },
  };
}
