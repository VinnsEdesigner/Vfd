/**
 * api-client layer public surface.
 *
 * Exposes a configured `Transport` plus helpers to build storage headers. The
 * rest layer is the only expected consumer.
 */

import type { StorageCredentials } from '../domain/media';
import { createTransport, type Transport, type TransportConfig } from './transport';

export { ApiError, NetworkError, isApiError, isNetworkError } from './errors';
export type { Transport, RequestOptions, TransportConfig } from './transport';

export interface ApiClient {
  transport: Transport;
}

export function createApiClient(config: TransportConfig): ApiClient {
  return { transport: createTransport(config) };
}

/**
 * Map domain storage credentials onto the wire headers the server expects.
 * Secrets live only in these headers and are never persisted client-side.
 */
export function storageHeaders(creds: StorageCredentials | undefined): Record<string, string> {
  if (!creds) return {};
  const h: Record<string, string> = {};
  if (creds.endpoint) h['X-S3-Endpoint'] = creds.endpoint;
  if (creds.region) h['X-S3-Region'] = creds.region;
  if (creds.bucket) h['X-S3-Bucket'] = creds.bucket;
  if (creds.accessKey) h['X-S3-Access-Key'] = creds.accessKey;
  if (creds.secretKey) h['X-S3-Secret-Key'] = creds.secretKey;
  if (creds.sessionToken) h['X-S3-Session-Token'] = creds.sessionToken;
  if (creds.cdnBase) h['X-Cdn-Base'] = creds.cdnBase;
  if (creds.cfToken) h['X-CF-Token'] = creds.cfToken;
  if (creds.cfZone) h['X-CF-Zone'] = creds.cfZone;
  if (creds.pathStyle !== undefined) h['X-S3-Path-Style'] = creds.pathStyle ? '1' : '0';
  return h;
}
