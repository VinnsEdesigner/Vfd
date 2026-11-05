/**
 * The extract normalizer.
 *
 * One call answers a URL and a search, so this returns one shape for both: a
 * single item carries qualities, a list carries entries. The reduction to one
 * quality per height is the same the crawl does, because the picker is the same
 * picker — a stream lists many variants of one size and showing them all is a
 * wall of near-duplicates.
 */

import { describe, expect, it } from 'vitest';
import { normalizeExtractResult } from './normalize';
import type { WireExtractResult } from './wire';

function wire(patch: Partial<WireExtractResult> = {}): WireExtractResult {
  return {
    ok: true,
    url: null,
    available: true,
    title: null,
    extractor: null,
    duration_seconds: -1,
    thumbnail: null,
    format_count: 0,
    entry_count: 0,
    entries: [],
    formats: [],
    ...patch,
  };
}

describe('normalizeExtractResult', () => {
  it('reduces a resolved item to one quality per height, largest first', () => {
    const out = normalizeExtractResult(
      wire({
        url: 'https://example.com/watch',
        title: 'A video',
        extractor: 'youtube',
        duration_seconds: 213,
        // Two 720p variants (a codec each) must collapse to one row.
        formats: [
          { url: 'u1', format_id: '1', ext: 'mp4', resolution: '1280x720', vcodec: 'avc1', acodec: 'none', mime: null, filesize: 1000, bitrate: 100, height: 720, has_video: true, has_audio: false },
          { url: 'u2', format_id: '2', ext: 'mp4', resolution: '1280x720', vcodec: 'vp9', acodec: 'none', mime: null, filesize: -1, bitrate: 90, height: 720, has_video: true, has_audio: false },
          { url: 'u3', format_id: '3', ext: 'mp4', resolution: '1920x1080', vcodec: 'avc1', acodec: 'none', mime: null, filesize: 5000, bitrate: 200, height: 1080, has_video: true, has_audio: false },
        ],
      }),
    );

    expect(out.qualities).toHaveLength(2);
    // Largest first, so the picker's default is the best.
    expect(out.qualities[0]!.height).toBe(1080);
    expect(out.qualities[0]!.best).toBe(true);
    expect(out.qualities[1]!.height).toBe(720);
    expect(out.qualities[1]!.best).toBe(false);
    // The size survives from whichever variant stated one.
    expect(out.qualities[1]!.bytes).toBe(1000);
    expect(out.title).toBe('A video');
    expect(out.durationSeconds).toBe(213);
    expect(out.entries).toEqual([]);
  });

  it('carries a list as entries, with no qualities', () => {
    const out = normalizeExtractResult(
      wire({
        extractor: 'youtube',
        entry_count: 2,
        entries: [
          { id: 'abc', title: 'One', url: 'https://example.com/1', uploader: 'Someone', thumbnail: null, duration_seconds: 635, is_playlist: false },
          { id: 'def', title: null, url: 'https://example.com/2', uploader: null, thumbnail: null, duration_seconds: -1, is_playlist: false },
        ],
      }),
    );

    expect(out.entries).toHaveLength(2);
    expect(out.qualities).toEqual([]);
    expect(out.entries[0]!.title).toBe('One');
    expect(out.entries[0]!.durationSeconds).toBe(635);
    // An entry with no title falls back to its id, so a row is never blank.
    expect(out.entries[1]!.title).toBe('def');
  });

  it('drops an entry with neither a title nor an id', () => {
    const out = normalizeExtractResult(
      wire({
        entries: [
          { id: null, title: null, url: 'https://example.com/x', uploader: null, thumbnail: null, duration_seconds: -1, is_playlist: false },
          { id: 'keep', title: 'Keep me', url: 'https://example.com/y', uploader: null, thumbnail: null, duration_seconds: -1, is_playlist: false },
        ],
      }),
    );
    expect(out.entries).toHaveLength(1);
    expect(out.entries[0]!.title).toBe('Keep me');
  });

  it('ignores a format with no picture when building the picker', () => {
    // Audio-only formats have height 0 and must not become a "0p" row.
    const out = normalizeExtractResult(
      wire({
        formats: [
          { url: 'a', format_id: '140', ext: 'm4a', resolution: null, vcodec: 'none', acodec: 'mp4a', mime: null, filesize: 300, bitrate: 128, height: 0, has_video: false, has_audio: true },
        ],
      }),
    );
    expect(out.qualities).toEqual([]);
  });

  it('survives a result with neither formats nor entries', () => {
    const out = normalizeExtractResult(wire({ formats: undefined as never, entries: undefined as never }));
    expect(out.qualities).toEqual([]);
    expect(out.entries).toEqual([]);
  });
});
