/**
 * Browse source tests.
 *
 * The search talks to Wikimedia Commons, so the fetch is injected: these exercise
 * the real normalisation (which is where the bugs live) against the response
 * shape the API actually returns.
 */

import { describe, expect, it, vi } from 'vitest';
import { searchCommons, SUPPORTED_KINDS } from './browse';

/** One page in the shape the MediaWiki API returns. */
function page(over: Record<string, unknown> = {}) {
  return {
    pageid: 42,
    title: 'File:Cat on snow.jpg',
    imageinfo: [
      {
        url: 'https://upload.wikimedia.org/cat.jpg',
        descriptionurl: 'https://commons.wikimedia.org/wiki/File:Cat_on_snow.jpg',
        mime: 'image/jpeg',
        size: 8231099,
        width: 3000,
        height: 2000,
        thumburl: 'https://thumb.wikimedia.org/330px-cat.jpg',
        extmetadata: { LicenseShortName: { value: 'CC BY-SA 4.0' } },
        ...over,
      },
    ],
  };
}

function respond(body: unknown, ok = true, status = 200) {
  return vi.fn().mockResolvedValue({
    ok,
    status,
    json: () => Promise.resolve(body),
  }) as unknown as typeof fetch;
}

describe('searchCommons', () => {
  it('normalises a result into the domain shape', async () => {
    const fetchImpl = respond({ query: { pages: { 42: page() }, searchinfo: { totalhits: 7 } } });
    const res = await searchCommons({ term: 'cat', kind: 'all', page: 1, perPage: 24 }, { fetchImpl });

    expect(res.items).toHaveLength(1);
    const item = res.items[0]!;
    expect(item.title).toBe('Cat on snow'); // File: prefix and extension dropped
    expect(item.filename).toBe('Cat on snow.jpg'); // the filename keeps them
    expect(item.kind).toBe('image');
    expect(item.bytes).toBe(8231099);
    expect(item.licence).toBe('CC BY-SA 4.0');
    expect(item.source).toBe('Wikimedia Commons');
    expect(item.thumbUrl).toContain('330px');
    expect(res.total).toBe(7);
  });

  it('strips HTML from the licence, which the API sometimes includes', async () => {
    const fetchImpl = respond({
      query: {
        pages: {
          42: page({
            extmetadata: { LicenseShortName: { value: '<a href="x">CC0</a>' } },
          }),
        },
      },
    });
    const res = await searchCommons({ term: 'cat', kind: 'all', page: 1, perPage: 24 }, { fetchImpl });
    expect(res.items[0]!.licence).toBe('CC0');
  });

  it('asks for a thumbnail width so the grid is not loading originals', async () => {
    const fetchImpl = respond({ query: { pages: {} } });
    await searchCommons({ term: 'cat', kind: 'all', page: 1, perPage: 24 }, { fetchImpl });
    const url = String((fetchImpl as unknown as { mock: { calls: unknown[][] } }).mock.calls[0]![0]);
    expect(url).toContain('iiurlwidth=360');
    // MediaWiki refuses a cross-origin browser call without this.
    expect(url).toContain('origin=*');
  });

  it('maps a kind filter onto a filetype search', async () => {
    const fetchImpl = respond({ query: { pages: {} } });
    await searchCommons({ term: 'birds', kind: 'audio', page: 1, perPage: 24 }, { fetchImpl });
    const url = String((fetchImpl as unknown as { mock: { calls: unknown[][] } }).mock.calls[0]![0]);
    // URLSearchParams encodes the space as '+', so compare on that form.
    expect(url).toContain('filetype%3Aaudio+birds');
  });

  it('does not search for an empty term', async () => {
    const fetchImpl = respond({ query: { pages: {} } });
    const res = await searchCommons({ term: '   ', kind: 'all', page: 1, perPage: 24 }, { fetchImpl });
    expect(res.items).toEqual([]);
    expect(fetchImpl).not.toHaveBeenCalled();
  });

  it('returns nothing for a kind the source cannot filter', async () => {
    // Commons has no "text" filetype, so claiming results would be a lie.
    const fetchImpl = respond({ query: { pages: {} } });
    const res = await searchCommons({ term: 'notes', kind: 'text', page: 1, perPage: 24 }, { fetchImpl });
    expect(res.items).toEqual([]);
    expect(fetchImpl).not.toHaveBeenCalled();
  });

  it('reports continuation as "has more"', async () => {
    const fetchImpl = respond({ query: { pages: { 42: page() } }, continue: { gsroffset: 24 } });
    const res = await searchCommons({ term: 'cat', kind: 'all', page: 1, perPage: 24 }, { fetchImpl });
    expect(res.hasMore).toBe(true);
  });

  it('paginates with an offset', async () => {
    const fetchImpl = respond({ query: { pages: {} } });
    await searchCommons({ term: 'cat', kind: 'all', page: 3, perPage: 10 }, { fetchImpl });
    const url = String((fetchImpl as unknown as { mock: { calls: unknown[][] } }).mock.calls[0]![0]);
    expect(url).toContain('gsroffset=20');
  });

  it('fails loudly on a bad response rather than showing an empty list', async () => {
    const fetchImpl = respond({}, false, 503);
    await expect(
      searchCommons({ term: 'cat', kind: 'all', page: 1, perPage: 24 }, { fetchImpl }),
    ).rejects.toThrow(/503/);
  });

  it('skips a page with no file URL instead of inventing one', async () => {
    const fetchImpl = respond({
      query: { pages: { 1: { pageid: 1, title: 'File:broken.jpg' }, 2: page() } },
    });
    const res = await searchCommons({ term: 'cat', kind: 'all', page: 1, perPage: 24 }, { fetchImpl });
    expect(res.items).toHaveLength(1);
    expect(res.items[0]!.id).toBe('42');
  });

  it('lists only the kinds it can actually find', () => {
    expect(SUPPORTED_KINDS).toContain('image');
    expect(SUPPORTED_KINDS).toContain('document');
    // No text, because Commons cannot filter for it.
    expect(SUPPORTED_KINDS).not.toContain('text');
  });
});
