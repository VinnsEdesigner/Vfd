/**
 * Integration test: the real data layer against the real C server.
 *
 * A helper (`tests/server-harness.ts`) starts the compiled `media-ingest`
 * binary on an ephemeral port, then these tests exercise api-client -> rest ->
 * domain end to end. No mocks: this is the actual HTTP API.
 */

import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { createRestApi, type RestApi } from '../src/rest';
import { ApiError } from '../src/api-client';
import { startServer, type ServerHandle } from './server-harness';

let server: ServerHandle;
let api: RestApi;

beforeAll(async () => {
  server = await startServer({ authToken: 'integration-token' });
  api = createRestApi({ baseUrl: server.baseUrl, authToken: 'integration-token' });
}, 30_000);

afterAll(async () => {
  await server?.stop();
});

describe('system resource', () => {
  it('reports health', async () => {
    await expect(api.system.health()).resolves.toBe(true);
  });

  it('fetches capabilities as a domain object', async () => {
    const caps = await api.system.capabilities();
    expect(caps.version).toBe('1');
    expect(caps.detectors).toContain('mp4');
    expect(caps.hls).toBe(true);
    expect(caps.upload).toBe(true);
  });

  it('reports the engine\u2019s own build and host facts', async () => {
    const caps = await api.system.capabilities();
    expect(caps.engineVersion).toBe('0.1.0');
    expect(caps.maxBodyBytes).toBe(64 * 1024 * 1024);
    expect(caps.workers).toBe(16);
    // ffmpeg presence is a property of the host, so the contract is that the
    // engine *reports* what it probed — not that this machine happens to have it.
    expect(typeof caps.ffmpeg).toBe('boolean');
  });

  it('names the directory the engine actually writes to', async () => {
    const store = await api.system.systemStorage();
    expect(store.workDir).toBe(server.workDir);
    expect(store.writable).toBe(true);
    expect(store.writeError).toBeNull();
  });

  it('moves the engine\u2019s directory when asked, and reports the new one', async () => {
    const target = `${server.workDir}-relocated`;
    await api.system.setLocation(target);
    const after = await api.system.systemStorage();
    expect(after.workDir).toBe(target);
    // Put it back so later cases see the original layout.
    await api.system.setLocation(server.workDir);
  });

  it('refuses a directory it cannot write to', async () => {
    // A path underneath a regular file can never be a directory.
    await expect(api.system.setLocation('/etc/hostname/child')).rejects.toBeInstanceOf(ApiError);
  });

  it('fetches the (empty) storage view', async () => {
    const cfg = await api.system.storageConfig();
    expect(cfg.configured).toBe(false);
    expect(cfg.bucket).toBeNull();
  });
});

describe('media resource', () => {
  it('detects a real MP4 from raw bytes', async () => {
    const bytes = makeMp4Header();
    const media = await api.media.detect(bytes);
    expect(media.kind).toBe('mp4');
    expect(media.isVideo).toBe(true);
    expect(media.container).toBe('mp4');
  });

  it('reports unknown for HTML bytes', async () => {
    const html = new TextEncoder().encode('<!DOCTYPE html><html>nope</html>');
    const media = await api.media.detect(html);
    expect(media.kind).toBe('unknown');
    expect(media.isVideo).toBe(false);
  });
});

describe('jobs resource', () => {
  it('starts empty', async () => {
    await expect(api.downloads.list()).resolves.toEqual([]);
  });

  it('creates a job, then reaches a terminal state', async () => {
    // A job pointed at an unreachable port fails fast; the point is the full
    // round trip through the API, not the download itself.
    const created = await api.downloads.create({
      url: 'http://127.0.0.1:1/nope.mp4',
      hls: false,
      upload: false,
      purge: false,
      connections: 2,
    });
    expect(created.id).toMatch(/^[0-9a-f]{32}$/);
    expect(['queued', 'running']).toContain(created.state);

    let job = created;
    for (let i = 0; i < 60 && !isSettled(job.state); i++) {
      await sleep(100);
      job = await api.downloads.get(created.id);
    }
    expect(job.state).toBe('failed');
    expect(job.error).toBeTruthy();
  });

  it('appears in the list after creation', async () => {
    const jobs = await api.downloads.list();
    expect(jobs.length).toBeGreaterThan(0);
  });

  it('returns 404 for an unknown job', async () => {
    await expect(api.downloads.get('deadbeef')).rejects.toBeInstanceOf(ApiError);
  });

  it('rejects a create without a url', async () => {
    await expect(
      api.downloads.create({ url: '', hls: false, upload: false, purge: false, connections: 1 }),
    ).rejects.toMatchObject({ status: 400 });
  });

  it('names a download after its source, not its id', async () => {
    const created = await api.downloads.create({
      url: 'http://127.0.0.1:1/holiday-clip.mp4',
      hls: false,
      upload: false,
      purge: false,
      connections: 1,
    });
    expect(created.name).toBe('holiday-clip.mp4');
    const fetched = await api.downloads.get(created.id);
    expect(fetched.name).toBe('holiday-clip.mp4');
  });

  it('renames a download through the API', async () => {
    const created = await api.downloads.create({
      url: 'http://127.0.0.1:1/rename-me.mp4',
      hls: false,
      upload: false,
      purge: false,
      connections: 1,
    });
    const renamed = await api.downloads.rename(created.id, 'Better Name.mp4');
    expect(renamed.name).toBe('Better Name.mp4');
    await expect(api.downloads.get(created.id)).resolves.toMatchObject({
      name: 'Better Name.mp4',
    });
  });

  it('rejects a rename that sanitises away to nothing', async () => {
    const created = await api.downloads.create({
      url: 'http://127.0.0.1:1/traverse.mp4',
      hls: false,
      upload: false,
      purge: false,
      connections: 1,
    });
    // Only separators and dots: nothing usable survives, so it is a bad request
    // rather than a silently mangled name.
    await expect(api.downloads.rename(created.id, '../../')).rejects.toMatchObject({
      status: 400,
    });
  });

  it('strips path separators from a rename instead of escaping the folder', async () => {
    const created = await api.downloads.create({
      url: 'http://127.0.0.1:1/escape.mp4',
      hls: false,
      upload: false,
      purge: false,
      connections: 1,
    });
    const renamed = await api.downloads.rename(created.id, '../../etc/passwd.mp4');
    expect(renamed.name).toBe('etcpasswd.mp4');
  });

  it('moves a download into a library folder', async () => {
    const created = await api.downloads.create({
      url: 'http://127.0.0.1:1/move-me.mp4',
      hls: false,
      upload: false,
      purge: false,
      connections: 1,
    });
    // The record survives the move; the bytes live under the new folder.
    const moved = await api.downloads.move(created.id, 'Archive');
    expect(moved.id).toBe(created.id);
    await expect(api.downloads.get(created.id)).resolves.toMatchObject({ id: created.id });
  });

  it('reports 404 when renaming an unknown download', async () => {
    await expect(api.downloads.rename('deadbeef', 'x.mp4')).rejects.toMatchObject({
      status: 404,
    });
  });
});

describe('auth', () => {
  it('fails without a token', async () => {
    const unauthed = createRestApi({ baseUrl: server.baseUrl });
    await expect(unauthed.downloads.list()).rejects.toMatchObject({ status: 401 });
  });

  it('still allows health without a token', async () => {
    const unauthed = createRestApi({ baseUrl: server.baseUrl });
    await expect(unauthed.system.health()).resolves.toBe(true);
  });
});

/* ---------------------------------------------------------------- */

function isSettled(state: string): boolean {
  return state === 'done' || state === 'failed' || state === 'cancelled';
}

function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}

/** A structurally valid ISO-BMFF header (ftyp/isom) so the detector says mp4. */
function makeMp4Header(): Uint8Array {
  const brand = new TextEncoder().encode('ftypisom');
  const body = new TextEncoder().encode('isomiso2mp41');
  const boxLen = 4 + brand.length + 4 + body.length;
  const out = new Uint8Array(boxLen + 4096);
  const view = new DataView(out.buffer);
  view.setUint32(0, boxLen, false);
  out.set(brand, 4);
  view.setUint32(12, 512, false);
  out.set(body, 16);
  return out;
}
