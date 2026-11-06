/**
 * The sites this app can read, and what it can do with each.
 *
 * WHY THIS LIST IS SHORT AND HONEST
 *
 * yt-dlp has ~1,755 extractors and gallery-dl ~4,927, so "supports 6,000+ sites"
 * is true and useless. What a person needs to know is *which of the sites they
 * actually use* work, and whether they can type a name or must paste a URL.
 *
 * Those two answers differ, and the difference is not our choice:
 *
 *   search      the site exposes a search the extractor uses. yt-dlp supports
 *               this for YouTube and SoundCloud only — there is no Facebook,
 *               Instagram, TikTok, Reddit or X search, verified by running
 *               `fbsearch1:test` and getting "Unsupported url scheme".
 *   by URL      the extractor resolves a page you already have the address for.
 *               Available far more widely.
 *   sign-in     the site refuses an anonymous request. That is the site's own
 *               access control, not a gap in the code, and cookies are what
 *               answer it.
 *
 * So the list below says which of the three applies. Claiming "search TikTok by
 * name" would fail the first time anyone tried it, which is worse than saying so.
 */

/** What can be done with a site. */
export type SiteAccess =
  /** Search by name, and resolve a URL. */
  | 'search'
  /** Resolve a URL. No search exists for this site. */
  | 'url'
  /** Resolve a URL, but the site refuses an anonymous request — sign-in needed. */
  | 'signin';

export interface SiteEntry {
  name: string;
  access: SiteAccess;
  /** Which tool reads it, so a failure can be explained. */
  tool: 'video' | 'gallery';
}

/**
 * The sites people actually ask about, grouped by what works.
 *
 * Deliberately not a dump of the extractor lists: a list of 1,755 names answers
 * no question a user has. This is the short version, and it is accurate.
 */
export const KNOWN_SITES: ReadonlyArray<SiteEntry> = [
  // Searchable by name: a term is enough, no URL needed.
  { name: 'YouTube', access: 'search', tool: 'video' },
  { name: 'SoundCloud', access: 'search', tool: 'video' },
  { name: 'Bilibili', access: 'search', tool: 'video' },

  // By URL only: these have extractors, but no search the extractor can use.
  { name: 'Vimeo', access: 'signin', tool: 'video' },
  { name: 'Dailymotion', access: 'url', tool: 'video' },
  { name: 'Twitch', access: 'url', tool: 'video' },
  { name: 'TikTok', access: 'signin', tool: 'video' },
  { name: 'Reddit', access: 'signin', tool: 'video' },
  { name: 'X / Twitter', access: 'signin', tool: 'video' },
  { name: 'Facebook', access: 'signin', tool: 'video' },
  { name: 'Instagram', access: 'signin', tool: 'video' },
  { name: 'archive.org', access: 'url', tool: 'video' },

  // Galleries and image hosts, read by gallery-dl.
  { name: 'Flickr', access: 'url', tool: 'gallery' },
  { name: 'Imgur', access: 'url', tool: 'gallery' },
  { name: 'DeviantArt', access: 'url', tool: 'gallery' },
  { name: 'ArtStation', access: 'url', tool: 'gallery' },
  { name: 'Pixiv', access: 'url', tool: 'gallery' },
  { name: 'Unsplash', access: 'url', tool: 'gallery' },
  { name: 'Pinterest', access: 'url', tool: 'gallery' },
  { name: 'Tumblr', access: 'url', tool: 'gallery' },
];

/** One line per access kind, for the summary. */
export function accessLabel(access: SiteAccess): string {
  switch (access) {
    case 'search':
      return 'search by name or URL';
    case 'url':
      return 'by URL';
    case 'signin':
      return 'by URL, may need sign-in';
    default:
      return '';
  }
}

/** Whether a site can be found without knowing its address. */
export function canSearch(site: SiteEntry): boolean {
  return site.access === 'search';
}
