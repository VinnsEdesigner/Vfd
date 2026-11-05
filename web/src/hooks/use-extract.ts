/**
 * The media extractor, as a mutation.
 *
 * An action rather than a query, for the same reason the crawl is: it costs
 * seconds and reads someone else's site, so it happens when the user asks and
 * not on a mount or a refetch. React Query retrying it would re-run a subprocess
 * for a failure that is almost always permanent — a site needing cookies, a
 * video that was taken down.
 *
 * The *same* hook serves all three uses, because they are one call: resolve a
 * page, search by name, or list a channel. A caller picks by which field it sets
 * on the query.
 */

import { useMutation } from '@tanstack/react-query';
import { useMediaApi } from './provider';
import type { ExtractQuery, ExtractResult } from '../domain/browse';

export interface ExtractVariables extends ExtractQuery {
  /** Cancels a resolve in flight; a Stop button uses it. */
  signal?: AbortSignal;
}

export function useExtract() {
  const { api } = useMediaApi();
  return useMutation<ExtractResult, unknown, ExtractVariables>({
    mutationFn: ({ signal, ...query }) => api.extract.run(query, signal),
    retry: false,
  });
}
