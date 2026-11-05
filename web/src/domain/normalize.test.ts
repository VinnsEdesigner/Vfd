/**
 * Domain normalizer tests. Pure functions, no network.
 */

import { describe, expect, it } from 'vitest';
import {
  nameFromUrl,
  normalizeSystemStorage,
  normalizeCapabilities,
  normalizeDownload,
  normalizeDownloadList,
  normalizeBrowseResult,
  normalizeLiveMessage,
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

describe('nameFromUrl', () => {
  it('takes the last path segment, ignoring query and fragment', () => {
    expect(nameFromUrl('http://x/a/b/clip.mp4?sig=1')).toBe('clip.mp4');
    expect(nameFromUrl('http://x/clip.mp4#t=3')).toBe('clip.mp4');
    expect(nameFromUrl('http://x/clip.mp4')).toBe('clip.mp4');
  });

  it('falls back to "download" when there is no segment', () => {
    expect(nameFromUrl('http://x/')).toBe('download');
    expect(nameFromUrl('')).toBe('download');
  });
});

describe('normalizeSystemStorage', () => {
  const wire = {
    work_dir: '/srv/library',
    mount_point: '/srv',
    fs_type: 'ext4',
    fs_device: '/dev/nvme0n1p2',
    filesystem: { total_bytes: 100, free_bytes: 40, avail_bytes: 30, used_bytes: 60 },
    library: { bytes: 10, files: 2 },
    downloads: { completed: 1, active: 0, failed: 0, bytes_transferred: 5 },
  };

  it('carries the volume identity through', () => {
    const s = normalizeSystemStorage(wire);
    expect(s.mountPoint).toBe('/srv');
    expect(s.fsType).toBe('ext4');
    expect(s.fsDevice).toBe('/dev/nvme0n1p2');
  });

  it('reports null rather than guessing when the server omits it', () => {
    const s = normalizeSystemStorage({ ...wire, mount_point: null, fs_type: null, fs_device: null });
    expect(s.mountPoint).toBeNull();
    expect(s.fsType).toBeNull();
  });

  it('handles a server that predates the fields entirely', () => {
    const { mount_point, fs_type, fs_device, ...old } = wire;
    void mount_point; void fs_type; void fs_device;
    const s = normalizeSystemStorage(old);
    expect(s.mountPoint).toBeNull();
    expect(s.workDir).toBe('/srv/library');
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

  it('uses the server-supplied name when there is one', () => {
    const job = normalizeDownload({ ...wire, name: 'Renamed Clip.mp4' });
    expect(job.name).toBe('Renamed Clip.mp4');
  });

  it('carries the on-disk filename through', () => {
    const job = normalizeDownload({ ...wire, name: 'holiday clip', filename: 'holiday clip.mp4' });
    expect(job.name).toBe('holiday clip');
    expect(job.filename).toBe('holiday clip.mp4');
  });

  it('falls back to the display name when the server sends no filename', () => {
    const job = normalizeDownload({ ...wire, name: 'y.mp4' });
    expect(job.filename).toBe('y.mp4');
  });

  it('derives a name from the url when the server omits it', () => {
    const job = normalizeDownload(wire);
    expect(job.name).toBe('y.mp4');
  });

  it('treats a blank server name as absent', () => {
    const job = normalizeDownload({ ...wire, name: '   ' });
    expect(job.name).toBe('y.mp4');
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

  it('treats a Vyzor that did not report live sessions as unable to', () => {
    const c = normalizeCapabilities({
      version: '1',
      detectors: [],
      hls: false,
      upload: false,
      purge: false,
    });
    expect(c.interactive).toBeNull();
  });

  it('maps a reported live-session capability', () => {
    const c = normalizeCapabilities({
      version: '1',
      detectors: [],
      hls: false,
      upload: false,
      purge: false,
      interactive: {
        enabled: true,
        available: true,
        max_concurrent: 2,
        active: 1,
        idle_timeout_seconds: 90,
      },
    });
    expect(c.interactive).toEqual({
      enabled: true,
      available: true,
      maxConcurrent: 2,
      active: 1,
      idleTimeoutSeconds: 90,
    });
  });

  it('fills in defaults for a missing field but keeps a reported zero', () => {
    const c = normalizeCapabilities({
      version: '1',
      detectors: [],
      hls: false,
      upload: false,
      purge: false,
      interactive: { enabled: true, available: false, max_concurrent: 1, active: 0 },
    });
    // A missing idle timeout takes the default.
    expect(c.interactive?.idleTimeoutSeconds).toBe(180);
    expect(c.interactive?.available).toBe(false);

    // A *reported* zero is meaningful — Vyzor uses it to mean "no idle
    // timeout" — so it must survive normalisation rather than become the default.
    const noIdle = normalizeCapabilities({
      version: '1',
      detectors: [],
      hls: false,
      upload: false,
      purge: false,
      interactive: { enabled: true, available: true, max_concurrent: 1, active: 0, idle_timeout_seconds: 0 },
    });
    expect(noIdle.interactive?.idleTimeoutSeconds).toBe(0);
  });
});

describe('normalizeLiveMessage', () => {
  it('maps ready, state and closed', () => {
    expect(normalizeLiveMessage({ type: 'ready', url: 'https://x/', width: 800, height: 600 })).toEqual({
      kind: 'ready',
      url: 'https://x/',
      width: 800,
      height: 600,
    });
    expect(
      normalizeLiveMessage({
        type: 'state',
        page: { url: 'https://x/a', title: 'A', loading: true },
      }),
    ).toEqual({ kind: 'state', page: { url: 'https://x/a', title: 'A', loading: true, editing: false } });
    expect(normalizeLiveMessage({ type: 'closed', reason: 'idle too long' })).toEqual({
      kind: 'closed',
      reason: 'idle too long',
    });
  });

  it('ignores a frame header, which is not a message on its own', () => {
    expect(normalizeLiveMessage({ type: 'frame', format: 'jpeg', seq: 1 })).toBeNull();
  });

  it('gives a reason when one is missing', () => {
    expect(normalizeLiveMessage({ type: 'closed' })).toEqual({
      kind: 'closed',
      reason: 'the session closed',
    });
    expect(normalizeLiveMessage({ type: 'error' })).toEqual({
      kind: 'error',
      message: 'the session reported an error',
    });
  });

  it('reports whether the page is ready for text', () => {
    // The console raises its on-screen keyboard on this flag, so a tap on a link
    // (editing false) must not open one and a tap on a field (true) must.
    const editing = normalizeLiveMessage({
      type: 'state',
      page: { url: 'https://x/', title: '', loading: false, editing: true },
    });
    expect(editing).toEqual({
      kind: 'state',
      page: { url: 'https://x/', title: '', loading: false, editing: true },
    });

    const notEditing = normalizeLiveMessage({
      type: 'state',
      page: { url: 'https://x/', title: '', loading: false },
    });
    expect(notEditing && notEditing.kind === 'state' && notEditing.page.editing).toBe(false);
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

describe('normalizeBrowseResult', () => {
  const wire = (patch: Partial<import('./wire').WireBrowseResult> = {}) => ({
    url: 'https://host.example/media/',
    title: 'My Gallery',
    items: [
      { url: 'https://host.example/media/clip.mp4', filename: 'clip.mp4', title: 'A clip', mime: 'video/mp4', bytes: 1234 },
      { url: 'https://host.example/media/photo.png', filename: 'photo.png', title: null, mime: 'image/png', bytes: -1 },
    ],
    count: 2,
    ...patch,
  });

  it('maps a listing onto candidates', () => {
    const r = normalizeBrowseResult(wire());
    expect(r.items).toHaveLength(2);
    expect(r.total).toBe(2);
    expect(r.title).toBe('My Gallery');
    const clip = r.items[0]!;
    expect(clip.filename).toBe('clip.mp4');
    // The link text wins over the filename: it is what the page calls it.
    expect(clip.title).toBe('A clip');
    expect(clip.kind).toBe('video');
    expect(clip.bytes).toBe(1234);
    // The page a person would visit is the listing itself.
    expect(clip.pageUrl).toBe('https://host.example/media/');
    expect(clip.source).toBe('host.example');
  });

  it('infers the kind from the mime, and falls back to the extension', () => {
    const r = normalizeBrowseResult(
      wire({
        items: [
          { url: 'https://h/a.bin', filename: 'a.mp3', title: null, mime: '', bytes: -1 },
          { url: 'https://h/b', filename: 'b', title: null, mime: 'image/png', bytes: -1 },
        ],
        count: 2,
      }),
    );
    expect(r.items[0]!.kind).toBe('audio');
    expect(r.items[1]!.kind).toBe('image');
  });

  it('drops an entry with no url, and never invents a size', () => {
    const r = normalizeBrowseResult(
      wire({
        items: [
          { url: '', filename: 'x.mp4', title: null, mime: '', bytes: 5 },
          { url: 'https://h/ok.mp4', filename: 'ok.mp4', title: null, mime: '', bytes: -1 },
        ],
        count: 2,
      }),
    );
    expect(r.items).toHaveLength(1);
    expect(r.items[0]!.bytes).toBe(-1);
  });

  it('falls back to the filename when the page gave no link text', () => {
    const r = normalizeBrowseResult(
      wire({
        items: [{ url: 'https://h/thing.mp4', filename: 'thing.mp4', title: null, mime: '', bytes: -1 }],
        count: 1,
      }),
    );
    expect(r.items[0]!.title).toBe('thing');
  });

  it('handles a missing title and a missing item list', () => {
    const r = normalizeBrowseResult({ url: 'https://h/', title: null, items: [], count: 0 });
    expect(r.items).toEqual([]);
    expect(r.title).toBeNull();
    expect(r.hasMore).toBe(false);
  });
});
