/**
 * The extract normalizer.
 *
 * One call answers a URL and a search, so this returns one shape for both: a
 * single item carries qualities, a list carries entries. The reduction to one
 * quality per height is the same the crawl does, because the picker is the same
 * picker — a stream lists many variants of one size and showing them all is a
 * wall of near-duplicates.
 *
 * A *torrent* search is the same call with a different index, so its entries
 * carry the index's own fields — seeders, leechers, size, category. The one
 * distinction the normalizer must keep is -1 ("not reported") from 0 ("none"):
 * a torrent with no seeders is dead, one whose seeders were not reported is
 * merely unknown, and the UI says different things for each.
 */

import { describe, expect, it } from 'vitest';
import { normalizeExtractResult, normalizeTorrentInfo } from './normalize';
import type { WireExtractResult, WireTorrentInfo } from './wire';

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

  it('carries a torrent entry\'s index fields, keeping -1 distinct from 0', () => {
    const out = normalizeExtractResult(
      wire({
        entries: [
          {
            id: 'abc',
            title: 'Ubuntu 24.04',
            url: 'magnet:?xt=urn:btih:abc',
            uploader: 'uploader',
            thumbnail: null,
            duration_seconds: -1,
            is_playlist: false,
            seeders: 0,
            leechers: 12,
            bytes: 1500000000,
            category: 'Applications',
            size_text: '1.4 GB',
            uploaded_text: '3 years ago',
            page_url: 'https://1337x.to/torrent/1/',
            is_torrent: true,
          },
        ],
      }),
    );
    const e = out.entries[0]!;
    // Zero seeders is a fact — a dead torrent — and must survive as 0.
    expect(e.seeders).toBe(0);
    expect(e.leechers).toBe(12);
    expect(e.bytes).toBe(1500000000);
    expect(e.category).toBe('Applications');
    expect(e.sizeText).toBe('1.4 GB');
    expect(e.uploadedText).toBe('3 years ago');
    expect(e.pageUrl).toBe('https://1337x.to/torrent/1/');
    expect(e.isTorrent).toBe(true);
  });

  it('reads a video entry\'s missing torrent fields as -1, not 0', () => {
    const out = normalizeExtractResult(
      wire({
        entries: [
          {
            id: 'v1',
            title: 'A video',
            url: 'https://example.com/watch',
            uploader: null,
            thumbnail: null,
            duration_seconds: 100,
            is_playlist: false,
          },
        ],
      }),
    );
    const e = out.entries[0]!;
    // "Not reported" must not read as "zero seeders" — a video has no seeders at
    // all, which is different from a torrent with none.
    expect(e.seeders).toBe(-1);
    expect(e.leechers).toBe(-1);
    expect(e.bytes).toBe(-1);
    expect(e.isTorrent).toBe(false);
    expect(e.category).toBeNull();
  });
});

describe('normalizeTorrentInfo', () => {
  function info(patch: Partial<WireTorrentInfo> = {}): WireTorrentInfo {
    return { ok: true, complete: false, name: null, info_hash: null, magnet: null, ...patch };
  }

  it('reads a .torrent file\'s whole structure, with each file\'s basename', () => {
    const out = normalizeTorrentInfo(
      info({
        complete: true,
        name: 'My Torrent',
        info_hash: 'abc123',
        total_bytes: 2048,
        piece_length: 16384,
        piece_count: 2,
        private: true,
        comment: 'a comment',
        created_by: 'mktorrent',
        created_at: 1700000000,
        files: [
          { path: 'My Torrent/Season 1/ep1.mkv', bytes: 1024 },
          { path: 'My Torrent/Season 1/ep2.mkv', bytes: 1024 },
        ],
        trackers: ['http://t.example/announce'],
      }),
    );
    expect(out.complete).toBe(true);
    expect(out.name).toBe('My Torrent');
    expect(out.totalBytes).toBe(2048);
    expect(out.pieceCount).toBe(2);
    expect(out.isPrivate).toBe(true);
    expect(out.files).toHaveLength(2);
    // The name is the last path segment, which is what a person reads.
    expect(out.files[0]!.name).toBe('ep1.mkv');
    expect(out.files[0]!.path).toBe('My Torrent/Season 1/ep1.mkv');
    expect(out.trackers).toEqual(['http://t.example/announce']);
  });

  it('carries the webseeds and the poster the server picked', () => {
    const out = normalizeTorrentInfo(
      info({
        complete: true,
        name: 'MyRelease',
        webseeds: ['http://seed.example/base/'],
        poster: 'http://seed.example/base/MyRelease/poster.png',
        files: [{ path: 'MyRelease/poster.png', bytes: 20070 }],
      }),
    );
    expect(out.webseeds).toEqual(['http://seed.example/base/']);
    // The poster is a webseed URL, because a torrent's images live inside it.
    expect(out.poster).toBe('http://seed.example/base/MyRelease/poster.png');
  });

  it('has no poster when the torrent carries none', () => {
    const out = normalizeTorrentInfo(info({ complete: true, name: 'x', poster: null }));
    expect(out.poster).toBeNull();
    expect(out.webseeds).toEqual([]);
  });

  it('reports a magnet as incomplete, with no files', () => {
    const out = normalizeTorrentInfo(
      info({
        complete: false,
        name: 'Ubuntu 24.04',
        info_hash: '0123456789abcdef0123456789abcdef01234567',
        magnet: 'magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567',
      }),
    );
    expect(out.complete).toBe(false);
    expect(out.files).toEqual([]);
    expect(out.infoHash).toBe('0123456789abcdef0123456789abcdef01234567');
    // The total is unknown for a magnet, not zero.
    expect(out.totalBytes).toBe(-1);
  });
});
