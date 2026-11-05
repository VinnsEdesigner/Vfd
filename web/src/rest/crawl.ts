/**
 * Site crawl: walk a site looking for files whose name matches a term.
 *
 * Goes through Vyzor rather than the browser, and has to: a crawl reads many
 * pages on another host, which CORS forbids from the page, and it may need to
 * drive a real browser for a site whose listing is built by JavaScript.
 *
 * A crawl is a *long* request — tens of seconds is normal, because it is reading
 * a site — so it takes an AbortSignal and the caller keeps the request alive
 * deliberately rather than treating slowness as a failure.
 */

import type { ApiClient } from '../api-client';
import { normalizeCrawlResult } from '../domain/normalize';
import type { CrawlQuery, CrawlResult } from '../domain/browse';
import type { WireCrawlRequest, WireCrawlResult } from '../domain/wire';

/**
 * How long the console waits before giving up on a crawl.
 *
 * A crawl is bounded by the *server's* own budget, which stops the walk, so this
 * is only the outer guard for a connection that has gone silent — set past the
 * server's limit so the server is what decides when to stop and the console gets
 * a real answer rather than an abort. It is kept close to the server's budget
 * because the user is waiting: a crawl that runs for minutes has already failed
 * to be useful, whatever it eventually finds.
 */
const CRAWL_TIMEOUT_MS = 120_000;
/** What the server is told it may spend. Below the timeout above, deliberately,
 *  so the server stops itself before the transport gives up on it. */
const CRAWL_BUDGET_SECONDS = 90;

export interface CrawlResource {
  /** Walk a site for files matching `term`. */
  run(query: CrawlQuery, signal?: AbortSignal): Promise<CrawlResult>;
}

export function createCrawlResource(client: ApiClient): CrawlResource {
  return {
    async run(query, signal) {
      const body: WireCrawlRequest = {
        url: query.url,
        ...(query.term ? { term: query.term } : {}),
        depth: query.depth,
        max_pages: query.maxPages,
        use_browser: query.useBrowser,
        max_browser_pages: query.maxBrowserPages,
        // A crawl is the one request worth waiting on, so the server's own
        // budget is what bounds it. Sending it explicitly means the two agree
        // rather than the server guessing from its default.
        budget_seconds: CRAWL_BUDGET_SECONDS,
      };

      /*
       * A crawl outlives the transport's default patience, so it gets its own
       * timeout rather than being aborted at the usual limit. `AbortSignal.any`
       * lets the caller cancel it too, which is what the Stop button needs.
       */
      const timeout = AbortSignal.timeout(CRAWL_TIMEOUT_MS);
      const combined = signal ? AbortSignal.any([signal, timeout]) : timeout;

      const wire = await client.transport.request<WireCrawlResult>({
        method: 'POST',
        path: '/api/v1/crawl',
        body,
        signal: combined,
      });
      return normalizeCrawlResult(wire);
    },
  };
}
