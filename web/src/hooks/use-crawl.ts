/**
 * A site crawl, as a mutation rather than a query.
 *
 * A crawl is an *action*: it costs tens of seconds and reads a site, so it
 * happens when the user asks and not on a mount or a refetch. Modelling it as a
 * query would mean React Query retrying it, caching it, and refetching it on
 * focus — all wrong for something with that cost and that side effect on
 * someone else's server.
 */

import { useMutation } from '@tanstack/react-query';
import { useMediaApi } from './provider';
import type { CrawlQuery, CrawlResult } from '../domain/browse';

export interface CrawlVariables extends CrawlQuery {
  /** Cancels a crawl in flight; the Stop button uses it. */
  signal?: AbortSignal;
}

export function useCrawl() {
  const { api } = useMediaApi();
  return useMutation<CrawlResult, unknown, CrawlVariables>({
    mutationFn: ({ signal, ...query }) =>
      api.crawl.run(query, signal),
    // A crawl is expensive and the user asked for it once. Retrying would
    // re-read the whole site for a failure that is almost always permanent
    // (a blocked host, a bad address), so it is not retried at all.
    retry: false,
  });
}
