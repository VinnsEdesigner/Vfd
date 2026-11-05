/**
 * In-app search.
 *
 * The search term is presentation state (it is what the user typed), so it lives
 * in the component; this hook only owns the request and its cache. That keeps the
 * debounce where the input is and the caching where the network is.
 */

import { useMutation, useQuery } from '@tanstack/react-query';
import { useMediaApi } from './provider';
import { queryKeys } from './query-keys';
import { searchCommons } from '../rest/browse';
import type {
  BrowseFilter,
  BrowseKind,
  BrowseResult,
  FetchedPage,
  RenderMode,
} from '../domain/browse';

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

/**
 * List the media at a URL the user typed.
 *
 * The request goes to Vyzor, not the browser: CORS stops the page fetching
 * an arbitrary host, while Vyzor can. Nothing is enabled until the URL
 * looks like an absolute one, so a half-typed address makes no request.
 */
/**
 * Whether a page can be shown inside the console, and where to point the frame.
 *
 * Vyzor fetches the page and reports the site's framing policy, because
 * only Vyzor can fetch another origin. A site that forbids framing is
 * reported as such rather than shown as an empty frame.
 */
export function usePageFrame(url: string | null) {
  const { api } = useMediaApi();
  const trimmed = (url ?? '').trim();
  const isAbsolute = /^https?:\/\/\S+$/i.test(trimmed);
  return useQuery<FetchedPage>({
    queryKey: queryKeys.pageFrame(trimmed),
    queryFn: ({ signal }) => api.page.inspect(trimmed, signal),
    enabled: isAbsolute,
    staleTime: 5 * 60_000,
    retry: 0,
  });
}

/**
 * Render a page with Vyzor's browser.
 *
 * A mutation rather than a query, because a render is an *action*: it costs
 * about half a gigabyte and a second or two, and it should happen when the user
 * asks rather than whenever a component mounts. Caching it as a query would
 * re-render on every remount, which is exactly what Vyzor's narrow gate
 * cannot afford.
 */
export function useRenderPage() {
  const { api } = useMediaApi();
  return useMutation({
    mutationFn: ({ url, mode }: { url: string; mode: RenderMode }) =>
      api.render.render(url, mode),
  });
}

export function useBrowseUrl(url: string | null, filter: BrowseFilter = 'media') {
  const { api } = useMediaApi();
  const trimmed = (url ?? '').trim();
  const isAbsolute = /^https?:\/\/\S+$/i.test(trimmed);
  return useQuery<BrowseResult>({
    queryKey: queryKeys.browseUrl(trimmed, filter),
    queryFn: ({ signal }) => api.browse.listFromUrl(trimmed, filter, signal),
    enabled: isAbsolute,
    staleTime: 5 * 60_000,
    retry: 0,
  });
}
