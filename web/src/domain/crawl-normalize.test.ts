/**
 * The crawl result normalizer.
 *
 * The wire names are snake_case and the counts arrive flat; this is the one
 * place that turns them into the shape the console uses. A field the server did
 * not send must become a neutral value rather than a guess — an absent score is
 * 0, not 50, so a result without one sorts last instead of looking mediocre.
 */

import { describe, expect, it } from 'vitest';
import { normalizeCrawlResult } from './normalize';
import type { WireCrawlResult } from './wire';

function wire(patch: Partial<WireCrawlResult> = {}): WireCrawlResult {
  return {
    ok: true,
    url: 'https://example.com/',
    term: 'stm32',
    found: 0,
    pages_read: 0,
    pages_failed: 0,
    pages_rendered: 0,
    skipped_offsite: 0,
    items: [],
    ...patch,
  };
}

describe('normalizeCrawlResult', () => {
  it('maps a hit and its provenance', () => {
    const out = normalizeCrawlResult(
      wire({
        found: 1,
        pages_read: 3,
        pages_rendered: 1,
        pages_failed: 1,
        skipped_offsite: 2,
        items: [
          {
            url: 'https://example.com/files/stm32f103.pdf',
            filename: 'stm32f103.pdf',
            title: 'STM32F103 datasheet',
            mime: 'application/pdf',
            score: 80,
            found_on: 'https://example.com/parts/',
            via_browser: true,
            resolved: false,
          },
        ],
      }),
    );

    expect(out.hits).toHaveLength(1);
    const hit = out.hits[0]!;
    expect(hit.url).toBe('https://example.com/files/stm32f103.pdf');
    expect(hit.filename).toBe('stm32f103.pdf');
    expect(hit.title).toBe('STM32F103 datasheet');
    expect(hit.score).toBe(80);
    expect(hit.foundOn).toBe('https://example.com/parts/');
    expect(hit.viaBrowser).toBe(true);

    // The flat counts are grouped, so a caller asks one object.
    expect(out.report).toEqual({
      pagesRead: 3,
      pagesFailed: 1,
      pagesRendered: 1,
      skippedOffsite: 2,
    });
  });


  it('carries the renditions of a resolved stream', () => {
    // A stream is a set of renditions, and the picker needs them in order with
    // their sizes. Absent qualities must be an empty list, not undefined, so the
    // UI can ask for `.length` without a guard.
    const out = normalizeCrawlResult(
      wire({
        items: [
          {
            url: 'https://example.com/watch',
            filename: 'A video',
            title: 'A video',
            mime: '',
            score: 80,
            found_on: 'https://example.com/watch',
            via_browser: false,
            resolved: true,
            qualities: [
              { height: 2160, label: '2160p', bytes: 358608461, best: true },
              { height: 720, label: '720p', bytes: 26455880, best: false },
            ],
          },
        ],
      }),
    );
    const hit = out.hits[0]!;
    expect(hit.resolved).toBe(true);
    expect(hit.qualities).toHaveLength(2);
    expect(hit.qualities[0]!.label).toBe('2160p');
    expect(hit.qualities[0]!.best).toBe(true);
    expect(hit.qualities[1]!.bytes).toBe(26455880);
  });

  it('gives an ordinary file an empty quality list rather than undefined', () => {
    const out = normalizeCrawlResult(
      wire({
        items: [
          {
            url: 'https://example.com/a.pdf',
            filename: 'a.pdf',
            title: null,
            mime: 'application/pdf',
            score: 80,
            found_on: null,
            via_browser: false,
            resolved: false,
          },
        ],
      }),
    );
    expect(out.hits[0]!.qualities).toEqual([]);
  });

  it('derives a filename when the server sent none', () => {
    const out = normalizeCrawlResult(
      wire({
        items: [
          {
            url: 'https://example.com/a/b/report.pdf',
            filename: '   ',
            title: null,
            mime: '',
            score: 10,
            found_on: null,
            via_browser: false,
            resolved: false,
          },
        ],
      }),
    );
    expect(out.hits[0]!.filename).toBe('report.pdf');
  });

  it('treats an empty title as absent rather than an empty string', () => {
    // A card shows the title when there is one and the filename otherwise, so
    // an empty string would render a blank heading.
    const out = normalizeCrawlResult(
      wire({
        items: [
          {
            url: 'https://example.com/x.pdf',
            filename: 'x.pdf',
            title: '',
            mime: '',
            score: 1,
            found_on: null,
            via_browser: false,
            resolved: false,
          },
        ],
      }),
    );
    expect(out.hits[0]!.title).toBeNull();
  });

  it('defaults an absent score to 0, not to a middling value', () => {
    const out = normalizeCrawlResult(
      wire({
        items: [
          {
            url: 'https://example.com/x.pdf',
            filename: 'x.pdf',
            title: null,
            mime: '',
            // score absent entirely
            found_on: null,
            via_browser: false,
            resolved: false,
          } as never,
        ],
      }),
    );
    expect(out.hits[0]!.score).toBe(0);
  });

  it('survives a result with no items at all', () => {
    const out = normalizeCrawlResult(wire({ items: undefined as never }));
    expect(out.hits).toEqual([]);
    expect(out.report.pagesRead).toBe(0);
  });

  it('coerces the browser flag to a boolean', () => {
    // The wire type is a boolean but the value is untyped JSON, so a 1 must not
    // reach the UI as a number.
    const out = normalizeCrawlResult(
      wire({
        items: [
          {
            url: 'https://example.com/x.pdf',
            filename: 'x.pdf',
            title: null,
            mime: '',
            score: 5,
            found_on: null,
            via_browser: 1 as never,
            resolved: false,
          },
        ],
      }),
    );
    expect(out.hits[0]!.viaBrowser).toBe(true);
  });
});
