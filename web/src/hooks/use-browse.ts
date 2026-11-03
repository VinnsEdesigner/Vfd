/**
 * In-app search.
 *
 * The search term is presentation state (it is what the user typed), so it lives
 * in the component; this hook only owns the request and its cache. That keeps the
 * debounce where the input is and the caching where the network is.
 */

import { useQuery } from '@tanstack/react-query';
import { queryKeys } from './query-keys';
import { searchCommons } from '../rest/browse';
import type { BrowseKind, BrowseResult } from '../domain/browse';

export interface UseBrowseArgs {
  term: string;
  kind: BrowseKind | 'all';
  page: number;
  perPage?: number;
}

export function useBrowse({ term, kind, page, perPage = 24 }: UseBrowseArgs) {
  const trimmed = term.trim();
  return useQuery<BrowseResult>({
    queryKey: queryKeys.browse(trimmed, kind, page, perPage),
    queryFn: ({ signal }) => searchCommons({ term: trimmed, kind, page, perPage }, { signal }),
    // Nothing to search for is not a request worth making.
    enabled: trimmed.length >= 2,
    // Results for a given term do not change often, and re-fetching on every
    // remount would be wasteful for a page the user is scrolling.
    staleTime: 5 * 60_000,
    retry: 1,
  });
}
