/**
 * rest layer public surface: a bundle of typed resources built over one client.
 */

import { createApiClient, type ApiClient, type TransportConfig } from '../api-client';
import {
  createBrowseResource,
  createPageResource,
  createRenderResource,
  createLiveResource,
  type BrowseResource,
  type PageResource,
  type RenderResource,
  type LiveResource,
} from './browse';
import { createRtcResource, type RtcResource } from './rtc';
import { createDownloadsResource, type DownloadsResource } from './downloads';
import { createMediaResource, type MediaResource } from './media';
import { createCrawlResource, type CrawlResource } from './crawl';
import { createSystemResource, type SystemResource } from './system';

export interface RestApi {
  client: ApiClient;
  downloads: DownloadsResource;
  media: MediaResource;
  system: SystemResource;
  /** Walk a site for files whose name matches a term. */
  crawl: CrawlResource;
  /** List the media a page or directory offers, via Vyzor. */
  browse: BrowseResource;
  /** Show a page itself, via Vyzor, when the site allows framing. */
  page: PageResource;
  /** Render a page with Vyzor's browser, when the host can. */
  render: RenderResource;
  /** Drive a page live with Vyzor's browser, over a WebSocket. */
  live: LiveResource;
  /** Stream a page as live video over WebRTC, signalled over HTTP. */
  rtc: RtcResource;
}

export function createRestApi(config: TransportConfig): RestApi {
  const client = createApiClient(config);
  return {
    client,
    downloads: createDownloadsResource(client),
    media: createMediaResource(client),
    system: createSystemResource(client),
    crawl: createCrawlResource(client),
    browse: createBrowseResource(client),
    page: createPageResource(client),
    render: createRenderResource(client),
    live: createLiveResource(client),
    rtc: createRtcResource(client),
  };
}

export type { TransportConfig } from '../api-client';
export type { BrowseResource, PageResource, RenderResource, LiveResource } from './browse';
export type { RtcResource } from './rtc';
export type { LiveVideoSession, LiveVideoHandlers, RtcSignalling } from './rtc-session';
export type { DownloadsResource } from './downloads';
export type { MediaResource } from './media';
export type { CrawlResource } from './crawl';
export type { SystemResource } from './system';
