/**
 * The media extractor: resolve a page, search by name, or list a channel.
 *
 * Three questions, one call, because they are the same operation from here — ask
 * the extractor and read the answer:
 *
 *   resolve   a page URL  -> the streams it plays, with their renditions
 *   search    a term      -> videos matching it, as a list
 *   list      a channel   -> its videos, as a list, without resolving each one
 *
 * The distinction that matters is *single item* versus *list*. A resolve
 * returns formats; a search or a list returns entries with titles and page URLs
 * and no stream URLs, because resolving a hundred items to show a hundred titles
 * would cost a hundred times as much for nothing. A chosen entry is resolved in
 * a second call, which is why `resolve` is used again for it.
 *
 * A resolve is slow — it is a subprocess against a remote site — so it takes an
 * AbortSignal and a generous timeout, the same way the crawl does.
 */

import type { ApiClient } from '../api-client';
import { normalizeExtractResult } from '../domain/normalize';
import type { ExtractQuery, ExtractResult } from '../domain/browse';
import type { WireExtractRequest, WireExtractResult } from '../domain/wire';

/**
 * How long the console waits on the extractor.
 *
 * A resolve is a network operation against a site that may be slow, and the
 * server has its own 60-second budget, so this sits just above it: the server
 * decides when to stop and the console only catches a connection that has gone
 * silent.
 */
const EXTRACT_TIMEOUT_MS = 90_000;

export interface ExtractResource {
  /** Resolve a page, search for a term, or list a channel. */
  run(query: ExtractQuery, signal?: AbortSignal): Promise<ExtractResult>;
  /** Whether the extractor is installed on that Vyzor. */
  available(): Promise<boolean>;
}

export function createExtractResource(client: ApiClient): ExtractResource {
  return {
    async run(query, signal) {
      const body: WireExtractRequest = {};
      if (query.url) body.url = query.url;
      if (query.search) body.search = query.search;
      /*
       * The index to search, for a torrent search with no URL. A URL routes by
       * its host, so this only matters for a bare term — which is exactly the
       * "just enter the filename" case.
       */
      if (query.site) body.site = query.site;
      if (query.limit !== undefined) body.limit = query.limit;
      if (query.enumerate) body.enumerate = true;
      /*
       * The filters are passed through in the extractor's own syntax.
       *
       * The console does not parse `duration > 600` or `1:20` and should not:
       * they are the extractor's language, and re-spelling them here would mean
       * tracking that language as it changes. The fields exist so a caller can
       * use them; they are not validated, because the extractor is the thing
       * that understands them.
       */
      if (query.dateAfter) body.date_after = query.dateAfter;
      if (query.dateBefore) body.date_before = query.dateBefore;
      if (query.playlistItems) body.playlist_items = query.playlistItems;
      if (query.matchFilter) body.match_filter = query.matchFilter;

      const timeout = AbortSignal.timeout(EXTRACT_TIMEOUT_MS);
      const combined = signal ? AbortSignal.any([signal, timeout]) : timeout;

      const wire = await client.transport.request<WireExtractResult>({
        method: 'POST',
        path: '/api/v1/extract',
        body,
        signal: combined,
      });
      return normalizeExtractResult(wire);
    },

    async available() {
      // The endpoint reports it on every answer; a bad address is the one case
      // that cannot be asked, and the caller treats an error as "no".
      const wire = await client.transport.request<WireExtractResult>({
        method: 'POST',
        path: '/api/v1/extract',
        body: { search: 'a', limit: 1 },
      });
      return Boolean(wire.available);
    },
  };
}
