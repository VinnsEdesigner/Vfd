/**
 * Domain normalizer tests. Pure functions, no network.
 */

import { describe, expect, it } from 'vitest';
import {
  normalizeCapabilities,
  normalizeDownload,
  normalizeDownloadList,
  normalizeMedia,
  normalizeProbe,
  toDownloadState,
  toMediaKind,
} from './normalize';
import { progressFraction, isTerminal, type Download } from './media';
import type { WireDownload } from './wire';

describe('toMediaKind', () => {
  it('passes known kinds through', () => {
    expect(toMediaKind('mp4')).toBe('mp4');
    expect(toMediaKind('mpeg-ts')).toBe('mpeg-ts');
  });

  it('maps anything unknown to "unknown"', () => {
    expect(toMediaKind('quicktime')).toBe('unknown');
    expect(toMediaKind(null)).toBe('unknown');
    expect(toMediaKind(undefined)).toBe('unknown');
  });
});

describe('toDownloadState', () => {
  it('accepts known states', () => {
    for (const s of ['queued', 'running', 'done', 'failed', 'cancelled']) {
      expect(toDownloadState(s)).toBe(s);
    }
  });

  it('treats an unrecognised state as failed rather than trusting it', () => {
    expect(toDownloadState('exploded')).toBe('failed');
  });
});

describe('normalizeMedia', () => {
  it('returns null for null input', () => {
    expect(normalizeMedia(null)).toBeNull();
  });

  it('maps snake_case to camelCase', () => {
    const m = normalizeMedia({ kind: 'mp4', container: 'mp4', confidence: 0.97, is_video: true });
    expect(m).toMatchObject({ kind: 'mp4', container: 'mp4', confidence: 0.97, isVideo: true });
    expect(m?.isImage).toBe(false);
    expect(m?.isAudio).toBe(false);
    expect(m?.qualities).toEqual([]);
  });

  it('defaults missing fields safely', () => {
    const m = normalizeMedia({ kind: 'weird' } as never);
    expect(m?.kind).toBe('unknown');
    expect(m?.container).toBe('');
    expect(m?.confidence).toBe(0);
    expect(m?.isVideo).toBe(false);
  });
});

describe('normalizeDownload', () => {
  const wire: WireDownload = {
    id: 'abc',
    url: 'http://x/y.mp4',
    state: 'done',
    progress: { downloaded_bytes: 100, total_bytes: 200 },
    media: { kind: 'mp4', container: 'mp4', confidence: 1, is_video: true },
    bytes: 200,
    sha256: 'deadbeef',
    master_playlist: '/tmp/hls/master.m3u8',
    artifacts: ['https://cdn/a.m3u8'],
    error: null,
    created_at: 1700000000,
    finished_at: 1700000010,
  };

  it('maps the full record', () => {
    const job = normalizeDownload(wire);
    expect(job.id).toBe('abc');
    expect(job.state).toBe('done');
    expect(job.progress).toEqual({ downloadedBytes: 100, totalBytes: 200 });
    expect(job.masterPlaylist).toBe('/tmp/hls/master.m3u8');
    expect(job.artifacts).toEqual(['https://cdn/a.m3u8']);
    expect(job.media?.isVideo).toBe(true);
  });

  it('survives a sparse record (the C writer emits nulls)', () => {
    const sparse = {
      id: 'x',
      url: 'u',
      state: 'queued',
    } as unknown as WireDownload;
    const job = normalizeDownload(sparse);
    expect(job.progress).toEqual({ downloadedBytes: 0, totalBytes: 0 });
    expect(job.artifacts).toEqual([]);
    expect(job.media).toBeNull();
    expect(job.error).toBeNull();
  });

  it('coerces a non-array artifacts field to an empty list', () => {
    const weird = { ...wire, artifacts: null } as unknown as WireDownload;
    expect(normalizeDownload(weird).artifacts).toEqual([]);
  });
});

describe('normalizeDownloadList', () => {
  it('maps items', () => {
    const list = normalizeDownloadList({
      items: [
        { id: 'a', url: 'u', state: 'running' } as unknown as WireDownload,
        { id: 'b', url: 'u', state: 'done' } as unknown as WireDownload,
      ],
      count: 2,
    });
    expect(list.map((j) => j.id)).toEqual(['a', 'b']);
  });

  it('handles a missing items array', () => {
    expect(normalizeDownloadList({} as never)).toEqual([]);
  });
});

describe('normalizeProbe', () => {
  it('maps the probe record and falls back finalUrl to url', () => {
    const p = normalizeProbe({
      url: 'http://a',
      final_url: null,
      content_type: null,
      total_bytes: -1,
      accept_ranges: false,
      media: null,
    });
    expect(p.finalUrl).toBe('http://a');
    expect(p.media).toBeNull();
    expect(p.acceptRanges).toBe(false);
  });
});

describe('normalizeCapabilities', () => {
  it('maps detectors and coerces unknown kinds', () => {
    const c = normalizeCapabilities({
      version: '1',
      detectors: ['mp4', 'mystery'],
      hls: true,
      upload: true,
      purge: false,
    });
    expect(c.detectors).toEqual(['mp4', 'unknown']);
    expect(c.hls).toBe(true);
    expect(c.purge).toBe(false);
  });
});

describe('domain helpers', () => {
  it('isTerminal covers the settled states', () => {
    expect(isTerminal('done')).toBe(true);
    expect(isTerminal('failed')).toBe(true);
    expect(isTerminal('cancelled')).toBe(true);
    expect(isTerminal('running')).toBe(false);
    expect(isTerminal('queued')).toBe(false);
  });

  it('progressFraction is clamped and safe on zero totals', () => {
    expect(progressFraction({ downloadedBytes: 0, totalBytes: 0 })).toBe(0);
    expect(progressFraction({ downloadedBytes: 50, totalBytes: 100 })).toBe(0.5);
    expect(progressFraction({ downloadedBytes: 200, totalBytes: 100 })).toBe(1);
    expect(progressFraction({ downloadedBytes: -5, totalBytes: 100 })).toBe(0);
  });

  it('every normalised download satisfies the domain type', () => {
    const download: Download = normalizeDownload({ id: 'a', url: 'u', state: 'queued' } as unknown as WireDownload);
    expect(download.id).toBe('a');
  });
});
