/**
 * View-model tests for the filename a save uses.
 *
 * A file saved without an extension cannot be opened by the OS that receives it,
 * so the extension is not cosmetic. These pin the three cases: Vyzor's
 * on-disk name wins, a name that already has an extension is left alone, and a
 * bare name gains the detected kind's extension.
 */

import { describe, expect, it } from 'vitest';
import { filenameFor, extensionOf, toDeviceView, kindFromMime } from './view-models';
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
  it('prefers the name Vyzor has on disk', () => {
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

describe('toDeviceView', () => {
  const file = {
    id: 'clip.mp4',
    name: 'clip.mp4',
    type: 'video/mp4',
    size: 2048,
    url: 'blob:vyzorix/1',
  };

  it('makes a device file a complete library entry', () => {
    const v = toDeviceView(file);
    expect(v.source).toBe('device');
    expect(v.localUrl).toBe('blob:vyzorix/1');
    expect(v.name).toBe('clip.mp4');
    // A stored file is finished by definition, and openable in the viewer.
    expect(v.state).toBe('done');
    expect(v.isOpenable).toBe(true);
    expect(v.isVideo).toBe(true);
    expect(v.bytesLabel).toBe('2.0 KB');
  });

  it('classifies audio and image files too', () => {
    expect(toDeviceView({ ...file, name: 'song.mp3', type: 'audio/mpeg' }).isAudio).toBe(true);
    expect(toDeviceView({ ...file, name: 'pic.png', type: 'image/png' }).isImage).toBe(true);
  });

  it('reads the extension when the OS did not supply a type', () => {
    const v = toDeviceView({ ...file, type: '', name: 'mystery.mkv' });
    expect(v.isVideo).toBe(true);
  });

  it('does not claim a kind it cannot tell', () => {
    const v = toDeviceView({ ...file, type: '', name: 'notes' });
    expect(v.isVideo).toBe(false);
    expect(v.isImage).toBe(false);
    expect(v.isAudio).toBe(false);
  });
});

describe('kindFromMime', () => {
  it('maps common types to a container', () => {
    expect(kindFromMime('video/mp4', 'a.mp4')).toBe('mp4');
    expect(kindFromMime('video/webm', 'a.webm')).toBe('matroska');
    expect(kindFromMime('audio/mpeg', 'a.mp3')).toBe('mp3');
    expect(kindFromMime('audio/wav', 'a.wav')).toBe('wav');
    expect(kindFromMime('image/png', 'a.png')).toBe('png');
    expect(kindFromMime('image/jpeg', 'a.jpg')).toBe('jpeg');
  });

  it('ignores a charset parameter', () => {
    expect(kindFromMime('video/mp4; codecs=avc1', 'a.mp4')).toBe('mp4');
  });

  it('falls back to the extension for an unknown type', () => {
    expect(kindFromMime('application/octet-stream', 'a.mkv')).toBe('mp4');
    expect(kindFromMime('', 'a.flac')).toBe('mp3');
    expect(kindFromMime('', 'a.jpeg')).toBe('jpeg');
  });

  it('admits when it cannot tell', () => {
    expect(kindFromMime('application/pdf', 'doc.pdf')).toBe('unknown');
  });
});
