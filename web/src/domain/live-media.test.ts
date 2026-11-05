/**
 * The media list's pure parts.
 *
 * Worth pinning because they decide what a person reads: a size that says
 * "unknown" rather than "-1", a format label that says "HLS" rather than a mime
 * type, and variants ordered best-first — a list of qualities is read from the
 * top, so the top had better be the one someone usually wants.
 */

import { describe, expect, it } from 'vitest';
import {
  filenameFromUrl,
  formatBandwidth,
  formatMediaSize,
  mediaFormatLabel,
  normalizeLiveMedia,
} from './live-media';

describe('normalizeLiveMedia', () => {
  it('reads a list Vyzor sent', () => {
    const items = normalizeLiveMedia({
      items: [
        { url: 'https://x/a.mp4', kind: 'video', mime: 'video/mp4', bytes: 2048, manifest: false },
      ],
    });
    expect(items).toHaveLength(1);
    expect(items[0]!.kind).toBe('video');
    expect(items[0]!.bytes).toBe(2048);
    expect(items[0]!.filename).toBe('a.mp4');
  });

  it('skips an entry with no url rather than taking the list with it', () => {
    const items = normalizeLiveMedia({
      items: [
        { url: '', kind: 'video' },
        { kind: 'video' },
        { url: 'https://x/ok.mp4', kind: 'video' },
      ],
    });
    expect(items).toHaveLength(1);
    expect(items[0]!.url).toBe('https://x/ok.mp4');
  });

  it('orders a manifest\u2019s variants best first', () => {
    const items = normalizeLiveMedia({
      items: [
        {
          url: 'https://x/m.m3u8',
          kind: 'manifest',
          manifest: true,
          variants: [
            { label: '480p', height: 480, bandwidth: 800000, url: 'a' },
            { label: '1080p', height: 1080, bandwidth: 6000000, url: 'b' },
            { label: '720p', height: 720, bandwidth: 2000000, url: 'c' },
          ],
        },
      ],
    });
    expect(items[0]!.variants.map((v) => v.label)).toEqual(['1080p', '720p', '480p']);
  });

  it('treats an unknown kind as video rather than dropping it', () => {
    const items = normalizeLiveMedia({ items: [{ url: 'https://x/y', kind: 'weird' }] });
    expect(items[0]!.kind).toBe('video');
  });

  it('tolerates a missing items array', () => {
    expect(normalizeLiveMedia({})).toEqual([]);
  });
});

describe('filenameFromUrl', () => {
  it('takes the last path segment and drops the query', () => {
    expect(filenameFromUrl('https://x/y/clip.mp4?token=abc&range=0-100')).toBe('clip.mp4');
  });

  it('decodes an escaped name', () => {
    expect(filenameFromUrl('https://x/a%20b.mp4')).toBe('a b.mp4');
  });

  it('falls back to the host when there is no path', () => {
    expect(filenameFromUrl('https://example.com/')).toBe('example.com');
    expect(filenameFromUrl('https://example.com')).toBe('example.com');
  });

  it('always returns something', () => {
    expect(filenameFromUrl('')).toBe('download');
    expect(filenameFromUrl('not a url')).toBe('download');
  });
});

describe('formatMediaSize', () => {
  it('says unknown rather than a negative number', () => {
    expect(formatMediaSize(-1)).toBe('size unknown');
  });

  it('reads as a person would', () => {
    expect(formatMediaSize(512)).toBe('512 B');
    expect(formatMediaSize(2048)).toBe('2.0 KB');
    expect(formatMediaSize(5 * 1024 * 1024)).toBe('5.0 MB');
    expect(formatMediaSize(1500 * 1024 * 1024)).toBe('1.5 GB');
  });
});

describe('formatBandwidth', () => {
  it('is empty when the playlist did not say', () => {
    expect(formatBandwidth(0)).toBe('');
  });

  it('reads as a person would', () => {
    expect(formatBandwidth(460560)).toBe('461 kbps');
    expect(formatBandwidth(6221600)).toBe('6.2 Mbps');
  });
});

describe('mediaFormatLabel', () => {
  it('names a manifest by its kind', () => {
    const [m] = normalizeLiveMedia({
      items: [{ url: 'https://x/a.m3u8', kind: 'manifest', manifest: true, mime: 'audio/mpegurl' }],
    });
    expect(mediaFormatLabel(m!)).toBe('HLS');
  });

  it('names a file by its extension, and by its mime when it has none', () => {
    const [a] = normalizeLiveMedia({ items: [{ url: 'https://x/a.webm', kind: 'video' }] });
    expect(mediaFormatLabel(a!)).toBe('WEBM');
    const [b] = normalizeLiveMedia({
      items: [{ url: 'https://x/stream', kind: 'video', mime: 'video/mp4' }],
    });
    expect(mediaFormatLabel(b!)).toBe('MP4');
  });
});
