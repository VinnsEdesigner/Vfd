/**
 * System resource: health, capabilities and the (non-secret) storage view.
 */

import type { ApiClient } from '../api-client';
import {
  normalizeCapabilities,
  normalizeStorageConfig,
  normalizeSystemStorage,
} from '../domain/normalize';
import type { Capabilities, StorageConfig, SystemStorage } from '../domain/media';
import type { WireCapabilities, WireSetLocationResult, WireStorageConfig, WireSystemStorage } from '../domain/wire';

export interface SystemResource {
  health(signal?: AbortSignal): Promise<boolean>;
  capabilities(signal?: AbortSignal): Promise<Capabilities>;
  storageConfig(signal?: AbortSignal): Promise<StorageConfig>;
  /** Real disk + library figures measured by the engine. */
  systemStorage(signal?: AbortSignal): Promise<SystemStorage>;
  /** Ask the engine to write downloads to `dir`. Rejects if it cannot. */
  setLocation(dir: string, signal?: AbortSignal): Promise<void>;
}

export function createSystemResource(client: ApiClient): SystemResource {
  return {
    async health(signal) {
      try {
        await client.transport.request<{ status: string }>({
          method: 'GET',
          path: '/healthz',
          ...(signal ? { signal } : {}),
        });
        return true;
      } catch {
        return false;
      }
    },

    async capabilities(signal) {
      const wire = await client.transport.request<WireCapabilities>({
        method: 'GET',
        path: '/api/v1/capabilities',
        ...(signal ? { signal } : {}),
      });
      return normalizeCapabilities(wire);
    },

    async storageConfig(signal) {
      const wire = await client.transport.request<WireStorageConfig>({
        method: 'GET',
        path: '/api/v1/storage/config',
        ...(signal ? { signal } : {}),
      });
      return normalizeStorageConfig(wire);
    },

    async systemStorage(signal) {
      const wire = await client.transport.request<WireSystemStorage>({
        method: 'GET',
        path: '/api/v1/system/storage',
        ...(signal ? { signal } : {}),
      });
      return normalizeSystemStorage(wire);
    },

    async setLocation(dir, signal) {
      await client.transport.request<WireSetLocationResult>({
        method: 'PUT',
        path: '/api/v1/storage/location',
        body: { dir },
        ...(signal ? { signal } : {}),
      });
    },
  };
}
