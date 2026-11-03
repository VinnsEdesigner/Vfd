/**
 * rest layer public surface: a bundle of typed resources built over one client.
 */

import { createApiClient, type ApiClient, type TransportConfig } from '../api-client';
import { createBrowseResource, createPageResource, type BrowseResource, type PageResource } from './browse';
import { createDownloadsResource, type DownloadsResource } from './downloads';
import { createMediaResource, type MediaResource } from './media';
import { createSystemResource, type SystemResource } from './system';

export interface RestApi {
  client: ApiClient;
  downloads: DownloadsResource;
  media: MediaResource;
  system: SystemResource;
  /** List the media a page or directory offers, via the engine. */
  browse: BrowseResource;
  /** Show a page itself, via the engine, when the site allows framing. */
  page: PageResource;
}

export function createRestApi(config: TransportConfig): RestApi {
  const client = createApiClient(config);
  return {
    client,
    downloads: createDownloadsResource(client),
    media: createMediaResource(client),
    system: createSystemResource(client),
    browse: createBrowseResource(client),
    page: createPageResource(client),
  };
}

export type { TransportConfig } from '../api-client';
export type { BrowseResource, PageResource } from './browse';
export type { DownloadsResource } from './downloads';
export type { MediaResource } from './media';
export type { SystemResource } from './system';
