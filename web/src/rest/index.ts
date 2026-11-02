/**
 * rest layer public surface: a bundle of typed resources built over one client.
 */

import { createApiClient, type ApiClient, type TransportConfig } from '../api-client';
import { createDownloadsResource, type DownloadsResource } from './downloads';
import { createMediaResource, type MediaResource } from './media';
import { createSystemResource, type SystemResource } from './system';

export interface RestApi {
  client: ApiClient;
  downloads: DownloadsResource;
  media: MediaResource;
  system: SystemResource;
}

export function createRestApi(config: TransportConfig): RestApi {
  const client = createApiClient(config);
  return {
    client,
    downloads: createDownloadsResource(client),
    media: createMediaResource(client),
    system: createSystemResource(client),
  };
}

export type { TransportConfig } from '../api-client';
export type { DownloadsResource } from './downloads';
export type { MediaResource } from './media';
export type { SystemResource } from './system';
