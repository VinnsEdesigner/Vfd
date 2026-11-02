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
}

function toWireRequest(input: CreateDownloadInput): WireCreateDownloadRequest {
  const req: WireCreateDownloadRequest = {
    url: input.url,
    hls: input.hls,
    upload: input.upload,
    purge: input.purge,
    connections: input.connections,
  };
  if (input.prefix) req.prefix = input.prefix;
  if (input.hlsDir) req.hls_dir = input.hlsDir;
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
  };
}
