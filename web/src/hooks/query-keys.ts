/**
 * Central query-key factory. Presentation-layer concern: the cache shape is a
 * UI decision, not a data-layer one.
 */

export const queryKeys = {
  health: () => ['health'] as const,
  capabilities: () => ['capabilities'] as const,
  storageConfig: () => ['storage', 'config'] as const,
  systemStorage: () => ['storage', 'system'] as const,
  downloads: () => ['downloads'] as const,
  downloadList: () => [...queryKeys.downloads(), 'list'] as const,
  download: (id: string) => [...queryKeys.downloads(), 'detail', id] as const,
  /** In-app search results. Keyed by term, kind and page so each page caches. */
  browse: (term: string, kind: string, page: number, perPage: number) =>
    ['browse', term.trim().toLowerCase(), kind, page, perPage] as const,
  /** A listing fetched from a URL the user typed. */
  browseUrl: (url: string) => ['browse', 'url', url.trim()] as const,
};
