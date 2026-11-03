/**
 * View-model tests for the filename a save uses.
 *
 * A file saved without an extension cannot be opened by the OS that receives it,
 * so the extension is not cosmetic. These pin the three cases: the engine's
 * on-disk name wins, a name that already has an extension is left alone, and a
 * bare name gains the detected kind's extension.
 */

import { describe, expect, it } from 'vitest';
import { filenameFor, extensionOf } from './view-models';
import type { Download } from '../domain/media';

function dl(patch: Partial<Download>): Download {
  return {
    id: 'a',
    name: 'clip',
    filename: 'clip',
    url: 'https://cdn.example.com/clip',
    state: 'done',
    progress: { downloadedBytes: 10, totalBytes: 10 },
    bytesPerSecond: 0,
    media: null,
    bytes: 10,
    sha256: null,
    masterPlaylist: null,
    artifacts: [],
    error: null,
    requestedQuality: null,
    connections: 8,
    hls: false,
    upload: false,
    purge: false,
    createdAt: 1,
    finishedAt: 2,
    ...patch,
  } as Download;
}

const mp4 = { kind: 'mp4', container: 'mp4', confidence: 1, isVideo: true } as Download['media'];

describe('filenameFor', () => {
  it('prefers the name the engine has on disk', () => {
    expect(filenameFor(dl({ name: 'holiday clip', filename: 'holiday clip.mp4' }))).toBe(
      'holiday clip.mp4',
    );
  });

  it('leaves an explicit extension alone', () => {
    expect(filenameFor(dl({ name: 'clip.webm', filename: 'clip.webm' }))).toBe('clip.webm');
  });

  it('adds the detected kind\u2019s extension when the name has none', () => {
    expect(filenameFor(dl({ name: 'realFru', filename: 'realFru', media: mp4 }))).toBe(
      'realFru.mp4',
    );
  });

  it('falls back to the url segment when there is no name at all', () => {
    const f = filenameFor(
      dl({ name: '', filename: '', url: 'https://cdn.example.com/clips/movie', media: mp4 }),
    );
    expect(f).toBe('movie.mp4');
  });

  it('does not invent an extension when the kind is unknown', () => {
    expect(filenameFor(dl({ name: 'mystery', filename: 'mystery', media: null }))).toBe('mystery');
  });
});

describe('extensionOf', () => {
  it('reads a trailing extension, case-insensitively', () => {
    expect(extensionOf('a.MP4')).toBe('mp4');
    expect(extensionOf('archive.tar.gz')).toBe('gz');
  });

  it('reports nothing for a dotless or trailing-dot name', () => {
    expect(extensionOf('realFru')).toBe('');
    expect(extensionOf('weird.')).toBe('');
    expect(extensionOf('.hidden')).toBe('');
  });
});
