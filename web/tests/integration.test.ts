/**
 * Integration test: the real data layer against the real C server.
 *
 * A helper (`tests/server-harness.ts`) starts the compiled `vyzor`
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

  it('reports Vyzor\u2019s own build and host facts', async () => {
    const caps = await api.system.capabilities();
    expect(caps.vyzorVersion).toBe('0.1.0');
    expect(caps.maxBodyBytes).toBe(64 * 1024 * 1024);
    expect(caps.workers).toBe(16);
    // ffmpeg presence is a property of the host, so the contract is that the
    // Vyzor *reports* what it probed — not that this machine happens to have it.
    expect(typeof caps.ffmpeg).toBe('boolean');
  });

  it('names the directory Vyzor actually writes to', async () => {
    const store = await api.system.systemStorage();
    expect(store.workDir).toBe(server.workDir);
    expect(store.writable).toBe(true);
    expect(store.writeError).toBeNull();
  });

  it('moves Vyzor\u2019s directory when asked, and reports the new one', async () => {
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

describe('egress settings', () => {
  it('starts with no cookies and no proxy', async () => {
    const s = await api.system.egressSettings();
    expect(s.cookieConfigured).toBe(false);
    expect(s.cookieNames).toEqual([]);
    expect(s.proxy).toBe('');
  });

  it('stores cookies and a proxy, and reports only the cookie names', async () => {
    const exportText =
      '# Netscape HTTP Cookie File\n' +
      '.example.com\tTRUE\t/\tTRUE\t1700000000\tsid\tSUPERSECRET\n';
    await api.system.setEgress({ cookies: exportText, proxy: 'http://127.0.0.1:8888' });

    const s = await api.system.egressSettings();
    expect(s.cookieConfigured).toBe(true);
    // The names are returned so the operator can confirm the right export...
    expect(s.cookieNames).toContain('sid');
    // ...but the value must never come back over the API.
    expect(JSON.stringify(s)).not.toContain('SUPERSECRET');
    expect(s.proxy).toBe('http://127.0.0.1:8888');
  });

  it('leaves the cookies alone when only the proxy changes', async () => {
    await api.system.setEgress({ proxy: 'socks5://127.0.0.1:1080' });
    const s = await api.system.egressSettings();
    expect(s.cookieConfigured).toBe(true);
    expect(s.proxy).toBe('socks5://127.0.0.1:1080');
  });

  it('clears the proxy with an empty string', async () => {
    await api.system.setEgress({ proxy: '' });
    const s = await api.system.egressSettings();
    expect(s.proxy).toBe('');
    expect(s.cookieConfigured).toBe(true);
  });

  it('refuses an export that is not Netscape format', async () => {
    await expect(
      api.system.setEgress({ cookies: 'not a tab-separated export' }),
    ).rejects.toMatchObject({ status: 422 });
  });

  it('clears the cookies with an empty string', async () => {
    await api.system.setEgress({ cookies: '' });
    const s = await api.system.egressSettings();
    expect(s.cookieConfigured).toBe(false);
    expect(s.cookieNames).toEqual([]);
  });

  it('sets and reports the identity, captcha and frontier settings', async () => {
    await api.system.setEgress({
      impersonate: 'chrome',
      identityRetries: 3,
      captchaProvider: 'stub',
      captchaApiKey: 'CAPTCHA_SECRET',
      frontierDbUrl: 'libsql://example.turso.io',
      frontierToken: 'FRONTIER_SECRET',
    });
    const s = await api.system.egressSettings();
    expect(s.impersonate).toBe('chrome');
    expect(s.identityRetries).toBe(3);
    expect(s.captchaProvider).toBe('stub');
    expect(s.captchaKeySet).toBe(true);
    expect(s.frontierDbUrl).toBe('libsql://example.turso.io');
    expect(s.frontierTokenSet).toBe(true);
    // Neither secret is ever returned over the API.
    expect(JSON.stringify(s)).not.toContain('CAPTCHA_SECRET');
    expect(JSON.stringify(s)).not.toContain('FRONTIER_SECRET');

    /*
     * These are server-wide settings on a server shared by every test in this
     * file, so they are put back: a retry count left at 3 would make a later
     * test's deliberately-unreachable URL retry with backoff and blow its
     * timeout. This is the same reason the location tests restore the directory.
     */
    await api.system.setEgress({
      impersonate: '',
      identityRetries: 0,
      captchaProvider: '',
      captchaApiKey: '',
      frontierDbUrl: '',
      frontierToken: '',
    });
  });
});

describe('browse resource', () => {
  it('rejects a non-http url without trying to fetch it', async () => {
    await expect(api.browse.listFromUrl('file:///etc/passwd')).rejects.toMatchObject({
      status: 400,
    });
  });

  it('reports a page it could not read, rather than an empty listing', async () => {
    // A port nothing is listening on: Vyzor cannot reach it, and must say so
    // instead of reporting "no media here".
    await expect(api.browse.listFromUrl('http://127.0.0.1:1/nothing')).rejects.toMatchObject({
      status: 502,
    });
  });

  it('lists the media a real directory offers', async () => {
    // A real HTTP server serving a directory listing, so the extraction runs
    // against real markup rather than a fixture.
    const { createServer } = await import('node:http');
    const server = createServer((req, res) => {
      if (req.url === '/media/') {
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(
          '<html><head><title>Index of /media</title></head><body>' +
            '<a href="clip.mp4">clip.mp4</a>' +
            '<a href="song.mp3">song.mp3</a>' +
            '<a href="notes.txt">notes.txt</a>' +
            '<a href="archive.zip">archive.zip</a>' +
            '<link rel="stylesheet" href="style.css">' +
            '</body></html>',
        );
      } else {
        res.writeHead(404);
        res.end();
      }
    });
    await new Promise<void>((r) => server.listen(0, '127.0.0.1', r));
    const addr = server.address();
    const port = typeof addr === 'object' && addr ? addr.port : 0;

    try {
      const result = await api.browse.listFromUrl(`http://127.0.0.1:${port}/media/`);
      const names = result.items.map((i) => i.filename);
      expect(names).toContain('clip.mp4');
      expect(names).toContain('song.mp3');
      // Text is a kind this app can show, so it is a candidate too.
      expect(names).toContain('notes.txt');
      // But a stylesheet and an archive are not media: the listing is not a
      // mirror of every link on the page.
      expect(names).not.toContain('style.css');
      expect(names).not.toContain('archive.zip');
      // Relative references are resolved against the page they came from.
      const clip = result.items.find((i) => i.filename === 'clip.mp4');
      expect(clip?.url).toBe(`http://127.0.0.1:${port}/media/clip.mp4`);
      expect(result.title).toBe('Index of /media');
    } finally {
      await new Promise<void>((r) => server.close(() => r()));
    }
  });

  it('lists the whole page when the "all" filter is asked for', async () => {
    const { createServer } = await import('node:http');
    const server = createServer((req, res) => {
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end(
        '<html><head><title>A site</title><link rel="stylesheet" href="/style.css"></head><body>' +
          '<a href="/about">About us</a>' +
          '<a href="clip.mp4">A clip</a>' +
          '</body></html>',
      );
    });
    await new Promise<void>((r) => server.listen(0, '127.0.0.1', r));
    const addr = server.address();
    const port = typeof addr === 'object' && addr ? addr.port : 0;
    const base = `http://127.0.0.1:${port}/`;

    try {
      const media = await api.browse.listFromUrl(base);
      const all = await api.browse.listFromUrl(base, 'all');

      // Media only: the clip, and nothing else.
      expect(media.items.map((i) => i.filename)).toEqual(['clip.mp4']);
      // Everything: the page's own links and assets too, which is the point —
      // the site can be read as a browser shows it.
      const allNames = all.items.map((i) => i.filename);
      expect(allNames).toContain('about');
      expect(allNames).toContain('style.css');
      expect(allNames).toContain('clip.mp4');
      expect(all.items.length).toBeGreaterThan(media.items.length);

      // The link text travels, so the listing reads like the page.
      expect(all.items.find((i) => i.filename === 'about')?.title).toBe('About us');
    } finally {
      await new Promise<void>((r) => server.close(() => r()));
    }
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

  it('reports no conversion until one is asked for', async () => {
    const created = await api.downloads.create({
      url: 'http://127.0.0.1:1/convert-me.mkv',
      hls: false,
      upload: false,
      purge: false,
      connections: 1,
    });
    expect(created.conversion).toEqual({ state: 'none', error: null });
  });

  it('refuses to convert a download that has not finished', async () => {
    const created = await api.downloads.create({
      url: 'http://127.0.0.1:1/still-going.mkv',
      hls: false,
      upload: false,
      purge: false,
      connections: 1,
    });
    // A record that has not completed has no bytes to convert, so Vyzor
    // answers with a conflict rather than starting a doomed ffmpeg.
    await expect(api.downloads.convert(created.id)).rejects.toMatchObject({
      status: 409,
    });
  });

  it('reports 404 when converting an unknown download', async () => {
    await expect(api.downloads.convert('deadbeef')).rejects.toMatchObject({ status: 404 });
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
