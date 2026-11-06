/**
 * System resource: health, capabilities and the (non-secret) storage view.
 */

import type { ApiClient } from '../api-client';
import {
  normalizeCapabilities,
  normalizeEgressSettings,
  normalizeStorageConfig,
  normalizeSystemStorage,
} from '../domain/normalize';
import type {
  Capabilities,
  EgressSettings,
  EgressUpdate,
  StorageConfig,
  SystemStorage,
} from '../domain/media';
import type {
  WireCapabilities,
  WireEgressSettings,
  WireEgressUpdate,
  WireSetLocationResult,
  WireStorageConfig,
  WireSystemStorage,
} from '../domain/wire';

export interface SystemResource {
  health(signal?: AbortSignal): Promise<boolean>;
  capabilities(signal?: AbortSignal): Promise<Capabilities>;
  storageConfig(signal?: AbortSignal): Promise<StorageConfig>;
  /** Real disk + library figures measured by Vyzor. */
  systemStorage(signal?: AbortSignal): Promise<SystemStorage>;
  /** Ask Vyzor to write downloads to `dir`. Rejects if it cannot. */
  setLocation(dir: string, signal?: AbortSignal): Promise<void>;
  /** The cookies and proxy Vyzor uses to reach a refusing host. */
  egressSettings(signal?: AbortSignal): Promise<EgressSettings>;
  /** Set cookies and/or proxy. A field left out is unchanged. */
  setEgress(next: EgressUpdate, signal?: AbortSignal): Promise<EgressSettings>;
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

    async egressSettings(signal) {
      const wire = await client.transport.request<WireEgressSettings>({
        method: 'GET',
        path: '/api/v1/settings/egress',
        ...(signal ? { signal } : {}),
      });
      return normalizeEgressSettings(wire);
    },

    async setEgress(next, signal) {
      // An omitted field is left alone by the server; an empty string clears.
      // The domain is camelCase and the wire is snake_case, so the mapping is
      // explicit here rather than spread over the callers.
      const body: WireEgressUpdate = {};
      if (next.cookies !== undefined) body.cookies = next.cookies;
      if (next.proxy !== undefined) body.proxy = next.proxy;
      if (next.impersonate !== undefined) body.impersonate = next.impersonate;
      if (next.identityRetries !== undefined) body.identity_retries = next.identityRetries;
      if (next.captchaProvider !== undefined) body.captcha_provider = next.captchaProvider;
      if (next.captchaApiKey !== undefined) body.captcha_api_key = next.captchaApiKey;
      if (next.captchaBaseUrl !== undefined) body.captcha_base_url = next.captchaBaseUrl;
      if (next.frontierDbUrl !== undefined) body.frontier_db_url = next.frontierDbUrl;
      if (next.frontierDbPath !== undefined) body.frontier_db_path = next.frontierDbPath;
      if (next.frontierToken !== undefined) body.frontier_token = next.frontierToken;
      const wire = await client.transport.request<WireEgressSettings>({
        method: 'PUT',
        path: '/api/v1/settings/egress',
        body,
        ...(signal ? { signal } : {}),
      });
      return normalizeEgressSettings(wire);
    },
  };
}
