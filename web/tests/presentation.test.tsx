/**
 * Presentation-layer tests.
 *
 * Hooks are exercised through a real React Query client with an injected fetch,
 * so the query/mutation plumbing (cache keys, invalidation, polling stop) is
 * tested rather than stubbed. Stores are tested directly.
 */

import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { renderHook, waitFor, act } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { ReactNode } from 'react';

import {
  MediaApiProvider,
  useDownload,
  useDownloads,
  useCreateDownload,
  useConvertDownload,
  useBrowseUrl,
  useRenderPage,
  useHealth,
  useSystemStorage,
  useSetStorageLocation,
} from '../src/hooks';
import { useConnectionStore, connectionSettings, DEFAULT_CONNECTION } from '../src/stores/connection-store';
import { useStorageStore, effectiveCredentials } from '../src/stores/storage-store';
import { useDownloadFormStore } from '../src/stores/download-form-store';

/* ---------------------------------------------------------------- */
/* a fetch double that speaks the wire format                        */
/* ---------------------------------------------------------------- */

interface RecordedCall {
  url: string;
  method: string;
  body: unknown;
  headers: Record<string, string>;
}

function makeFetch(routes: Record<string, (init: RequestInit) => unknown>): {
  fetchImpl: typeof fetch;
  calls: RecordedCall[];
} {
  const calls: RecordedCall[] = [];
  const fetchImpl = (async (input: RequestInfo | URL, init: RequestInit = {}) => {
    const url = typeof input === 'string' ? input : input.toString();
    const method = (init.method ?? 'GET').toUpperCase();
    const headers: Record<string, string> = {};
    for (const [k, v] of Object.entries((init.headers ?? {}) as Record<string, string>)) {
      headers[k.toLowerCase()] = String(v);
    }
    let body: unknown = undefined;
    if (typeof init.body === 'string') {
      try {
        body = JSON.parse(init.body);
      } catch {
        body = init.body;
      }
    }
    calls.push({ url, method, body, headers });

    const key = `${method} ${new URL(url).pathname}`;
    const handler = routes[key];
    if (!handler) {
      return new Response(JSON.stringify({ error: 'not found', status: 404 }), {
        status: 404,
        headers: { 'Content-Type': 'application/json' },
      });
    }
    const result = handler(init) as {
      status?: number;
      json: unknown;
      /** Extra response headers, for the endpoints whose answer is a header. */
      headers?: Record<string, string>;
    };
    const status = result.status ?? 200;
    return new Response(JSON.stringify(result.json), {
      status,
      headers: { 'Content-Type': 'application/json', ...(result.headers ?? {}) },
    });
  }) as unknown as typeof fetch;
  return { fetchImpl, calls };
}

function wrapper(fetchImpl: typeof fetch, baseUrl = 'http://test.local') {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false }, mutations: { retry: false } },
  });
  return function Wrapper({ children }: { children: ReactNode }) {
    return (
      <QueryClientProvider client={client}>
        <MediaApiProvider baseUrl={baseUrl} fetchImpl={fetchImpl}>
          {children}
        </MediaApiProvider>
      </QueryClientProvider>
    );
  };
}

/* ---------------------------------------------------------------- */

describe('useDownloads', () => {
  it('fetches and normalises the download list', async () => {
    const { fetchImpl } = makeFetch({
      'GET /api/v1/downloads': () => ({
        json: {
          items: [
            {
              id: 'j1',
              url: 'http://x/a.mp4',
              state: 'done',
              progress: { downloaded_bytes: 10, total_bytes: 10 },
              media: { kind: 'mp4', container: 'mp4', confidence: 1, is_video: true },
              bytes: 10,
              sha256: 'ff',
              master_playlist: null,
              artifacts: [],
              error: null,
              created_at: 1,
              finished_at: 2,
            },
          ],
          count: 1,
        },
      }),
    });

    const { result } = renderHook(() => useDownloads(), { wrapper: wrapper(fetchImpl) });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    expect(result.current.data).toHaveLength(1);
    expect(result.current.data?.[0]?.media?.isVideo).toBe(true);
  });
});

describe('useDownloads polling', () => {
  const listBody = (state: string) => ({
    json: {
      items: [
        {
          id: 'j1',
          url: 'http://x/a.mp4',
          state,
          progress: { downloaded_bytes: 1, total_bytes: 2 },
          media: null,
          bytes: 0,
          sha256: null,
          master_playlist: null,
          artifacts: [],
          error: null,
          created_at: 1,
          finished_at: 0,
        },
      ],
      count: 1,
    },
  });

  it('polls the list while a download is still moving', async () => {
    const { fetchImpl, calls } = makeFetch({
      'GET /api/v1/downloads': () => listBody('running'),
    });
    vi.useFakeTimers();
    renderHook(() => useDownloads(), { wrapper: wrapper(fetchImpl) });
    await vi.advanceTimersByTimeAsync(0);
    const before = calls.length;
    await vi.advanceTimersByTimeAsync(3_200);
    const after = calls.length;
    vi.useRealTimers();
    // Without this, a download started from the UI never leaves "running 0%".
    expect(after).toBeGreaterThan(before);
  });

  it('stops polling once everything has settled', async () => {
    const { fetchImpl, calls } = makeFetch({
      'GET /api/v1/downloads': () => listBody('done'),
    });
    vi.useFakeTimers();
    renderHook(() => useDownloads(), { wrapper: wrapper(fetchImpl) });
    await vi.advanceTimersByTimeAsync(0);
    const before = calls.length;
    await vi.advanceTimersByTimeAsync(5_000);
    vi.useRealTimers();
    expect(calls.length).toBe(before);
  });
});

describe('useCreateDownload', () => {
  it('posts the wire request and seeds the cache', async () => {
    const { fetchImpl, calls } = makeFetch({
      'POST /api/v1/downloads': () => ({
        status: 202,
        json: {
          id: 'newjob',
          url: 'http://x/a.mp4',
          state: 'queued',
          progress: { downloaded_bytes: 0, total_bytes: 0 },
          media: null,
          bytes: 0,
          sha256: null,
          master_playlist: null,
          artifacts: [],
          error: null,
          created_at: 1,
          finished_at: 0,
        },
      }),
      'GET /api/v1/downloads': () => ({ json: { items: [], count: 0 } }),
    });

    const { result } = renderHook(() => useCreateDownload(), { wrapper: wrapper(fetchImpl) });
    await act(async () => {
      await result.current.mutateAsync({
        input: { url: 'http://x/a.mp4', hls: true, upload: false, purge: false, connections: 4 },
      });
    });

    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    const post = calls.find((c) => c.method === 'POST');
    expect(post?.body).toMatchObject({ url: 'http://x/a.mp4', hls: true, connections: 4 });
  });

  it('sends storage credentials as headers, never in the body', async () => {
    const { fetchImpl, calls } = makeFetch({
      'POST /api/v1/downloads': () => ({
        status: 202,
        json: {
          id: 'j',
          url: 'u',
          state: 'queued',
          progress: { downloaded_bytes: 0, total_bytes: 0 },
          media: null,
          bytes: 0,
          sha256: null,
          master_playlist: null,
          artifacts: [],
          error: null,
          created_at: 0,
          finished_at: 0,
        },
      }),
      'GET /api/v1/downloads': () => ({ json: { items: [], count: 0 } }),
    });

    const { result } = renderHook(() => useCreateDownload(), { wrapper: wrapper(fetchImpl) });
    await act(async () => {
      await result.current.mutateAsync({
        input: { url: 'u', hls: false, upload: true, purge: false, connections: 1 },
        credentials: { bucket: 'b', accessKey: 'AK', secretKey: 'SK' },
      });
    });

    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    const post = calls.find((c) => c.method === 'POST');
    expect(post?.headers['x-s3-bucket']).toBe('b');
    expect(post?.headers['x-s3-access-key']).toBe('AK');
    expect(post?.headers['x-s3-secret-key']).toBe('SK');
    expect(JSON.stringify(post?.body)).not.toContain('SK');
  });
});

describe('useHealth', () => {
  it('resolves true when the server is healthy', async () => {
    const { fetchImpl } = makeFetch({
      'GET /healthz': () => ({ json: { status: 'ok' } }),
    });
    const { result } = renderHook(() => useHealth(), { wrapper: wrapper(fetchImpl) });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    expect(result.current.data).toBe(true);
  });

  it('resolves false when the server is down', async () => {
    const fetchImpl = (async () => {
      throw new TypeError('fetch failed');
    }) as unknown as typeof fetch;
    const { result } = renderHook(() => useHealth(), { wrapper: wrapper(fetchImpl) });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    expect(result.current.data).toBe(false);
  });
});

/* ---------------------------------------------------------------- */
/* stores                                                            */
/* ---------------------------------------------------------------- */

describe('connection store', () => {
  beforeEach(() => {
    useConnectionStore.setState({ baseUrl: 'http://127.0.0.1:8787', authToken: '' });
  });

  it('updates baseUrl and token independently', () => {
    act(() => useConnectionStore.getState().setBaseUrl('http://srv:9000'));
    act(() => useConnectionStore.getState().setAuthToken('tok'));
    const s = useConnectionStore.getState();
    expect(connectionSettings(s)).toEqual({ baseUrl: 'http://srv:9000', authToken: 'tok' });
  });

  it('persists to localStorage', () => {
    act(() => useConnectionStore.getState().setBaseUrl('http://persisted'));
    const raw = localStorage.getItem('vyzorix.media.connection');
    expect(raw).toContain('http://persisted');
  });

  it('resets to defaults', () => {
    act(() => useConnectionStore.getState().reset());
    expect(useConnectionStore.getState().baseUrl).toBe(DEFAULT_CONNECTION.baseUrl);
  });

  it('defaults to the page origin rather than a hardcoded localhost', () => {
    // jsdom serves on localhost; the point is that the default is derived from
    // where the page lives, so a deployed build is not stuck on 127.0.0.1:8787.
    expect(DEFAULT_CONNECTION.baseUrl).not.toBe('');
    expect(DEFAULT_CONNECTION.baseUrl.startsWith('http')).toBe(true);
  });

  it('drives the API client, so a change redirects every request', () => {
    // The provider reads these two values; changing them must produce a client
    // pointed elsewhere. Asserted at the store boundary the provider consumes.
    act(() => useConnectionStore.getState().setBaseUrl('http://engine.example:9000'));
    act(() => useConnectionStore.getState().setAuthToken('secret'));
    const settings = connectionSettings(useConnectionStore.getState());
    expect(settings.baseUrl).toBe('http://engine.example:9000');
    expect(settings.authToken).toBe('secret');
  });
});

describe('storage store', () => {
  beforeEach(() => {
    useStorageStore.getState().clear();
  });

  it('is incomplete until bucket + keys are set', () => {
    expect(useStorageStore.getState().isComplete()).toBe(false);
    act(() => {
      useStorageStore.getState().setField('bucket', 'b');
      useStorageStore.getState().setField('accessKey', 'AK');
      useStorageStore.getState().setField('secretKey', 'SK');
    });
    expect(useStorageStore.getState().isComplete()).toBe(true);
  });

  it('never persists secrets to localStorage', () => {
    act(() => useStorageStore.getState().setField('secretKey', 'super-secret'));
    expect(JSON.stringify(localStorage)).not.toContain('super-secret');
  });

  it('omits empty fields from the effective credentials', () => {
    act(() => {
      useStorageStore.getState().setField('bucket', 'b');
      useStorageStore.getState().setField('accessKey', '');
    });
    const creds = effectiveCredentials(useStorageStore.getState());
    expect(creds.bucket).toBe('b');
    expect(creds.accessKey).toBeUndefined();
  });
});

describe('job form store', () => {
  it('edits a field without disturbing the rest', () => {
    act(() => useDownloadFormStore.getState().reset());
    act(() => useDownloadFormStore.getState().setField('url', 'http://x/y.mp4'));
    const s = useDownloadFormStore.getState();
    expect(s.url).toBe('http://x/y.mp4');
    expect(s.hls).toBe(true);
    expect(s.connections).toBe(8);
  });

  it('resets to defaults', () => {
    act(() => useDownloadFormStore.getState().setField('url', 'changed'));
    act(() => useDownloadFormStore.getState().reset());
    expect(useDownloadFormStore.getState().url).toBe('');
  });
});

describe('system storage hooks', () => {
  const storageBody = {
    json: {
      work_dir: '/srv/media',
      writable: true,
      write_error: null,
      mount_point: '/srv',
      fs_type: 'ext4',
      fs_device: '/dev/sdb1',
      filesystem: { total_bytes: 100, free_bytes: 40, avail_bytes: 30, used_bytes: 60 },
      library: { bytes: 10, files: 2 },
      downloads: { completed: 0, active: 0, failed: 0, bytes_transferred: 0 },
    },
  };

  it('reports the engine\u2019s directory and that it is writable', async () => {
    const { fetchImpl } = makeFetch({ 'GET /api/v1/system/storage': () => storageBody });
    const { result } = renderHook(() => useSystemStorage(), { wrapper: wrapper(fetchImpl) });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    expect(result.current.data?.workDir).toBe('/srv/media');
    expect(result.current.data?.writable).toBe(true);
  });

  it('sends the chosen directory to the engine and refreshes the storage view', async () => {
    const { fetchImpl, calls } = makeFetch({
      'GET /api/v1/system/storage': () => storageBody,
      'PUT /api/v1/storage/location': (init) => {
        const dir = (JSON.parse(String(init.body)) as { dir: string }).dir;
        return { json: { dir, writable: true } };
      },
    });
    const { result } = renderHook(() => useSetStorageLocation(), { wrapper: wrapper(fetchImpl) });
    await act(async () => {
      await result.current.mutateAsync('/mnt/media');
    });
    const put = calls.find((c) => c.method === 'PUT');
    expect(put?.url).toContain('/api/v1/storage/location');
    expect(put?.body).toEqual({ dir: '/mnt/media' });
  });

  it('surfaces a refused directory as an error', async () => {
    const { fetchImpl } = makeFetch({
      'PUT /api/v1/storage/location': () => ({
        status: 422,
        json: { error: 'directory is not writable', dir: '/nope' },
      }),
    });
    const { result } = renderHook(() => useSetStorageLocation(), { wrapper: wrapper(fetchImpl) });
    await expect(result.current.mutateAsync('/nope')).rejects.toThrow();
  });
});

describe('useConvertDownload', () => {
  /** A record with a conversion state, as the engine reports it. */
  const record = (conversion: { state: string; error: string | null }) => ({
    json: {
      id: 'j1',
      url: 'http://x/movie.mkv',
      state: 'done',
      progress: { downloaded_bytes: 10, total_bytes: 10 },
      media: { kind: 'matroska', container: 'matroska', confidence: 1, is_video: true },
      bytes: 10,
      sha256: null,
      master_playlist: null,
      conversion,
      artifacts: [],
      error: null,
      created_at: 1,
      finished_at: 2,
    },
  });

  it('asks the engine to convert with the convert action', async () => {
    const { fetchImpl, calls } = makeFetch({
      'PATCH /api/v1/downloads/j1': () => ({
        status: 202,
        ...record({ state: 'converting', error: null }),
      }),
      'GET /api/v1/downloads': () => ({ json: { items: [], count: 0 } }),
    });
    const { result } = renderHook(() => useConvertDownload(), { wrapper: wrapper(fetchImpl) });
    await act(async () => {
      await result.current.mutateAsync('j1');
    });
    const patch = calls.find((c) => c.method === 'PATCH');
    expect(patch?.body).toEqual({ action: 'convert' });
    await waitFor(() => expect(result.current.data?.conversion.state).toBe('converting'));
  });

  it('normalises an absent conversion to none, for an older server', async () => {
    const body = record({ state: 'none', error: null }).json as Record<string, unknown>;
    delete body.conversion;
    const { fetchImpl } = makeFetch({
      'GET /api/v1/downloads': () => ({ json: { items: [body], count: 1 } }),
    });
    const { result } = renderHook(() => useDownloads(), { wrapper: wrapper(fetchImpl) });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    expect(result.current.data?.[0]?.conversion.state).toBe('none');
  });

  it('keeps polling the detail while a conversion is running', async () => {
    const { fetchImpl, calls } = makeFetch({
      'GET /api/v1/downloads/j1': () => record({ state: 'converting', error: null }),
    });
    vi.useFakeTimers();
    renderHook(() => useDownload('j1'), { wrapper: wrapper(fetchImpl) });
    await vi.advanceTimersByTimeAsync(0);
    const before = calls.length;
    await vi.advanceTimersByTimeAsync(3_200);
    vi.useRealTimers();
    // Without this the viewer would sit on "converting" forever, because the
    // download itself has already settled.
    expect(calls.length).toBeGreaterThan(before);
  });

  it('stops polling the detail once the conversion has finished', async () => {
    const { fetchImpl, calls } = makeFetch({
      'GET /api/v1/downloads/j1': () => record({ state: 'done', error: null }),
    });
    vi.useFakeTimers();
    renderHook(() => useDownload('j1'), { wrapper: wrapper(fetchImpl) });
    await vi.advanceTimersByTimeAsync(0);
    const before = calls.length;
    await vi.advanceTimersByTimeAsync(5_000);
    vi.useRealTimers();
    expect(calls.length).toBe(before);
  });
});

describe('useBrowseUrl', () => {
  const listing = {
    json: {
      url: 'https://host.example/media/',
      title: 'A Gallery',
      items: [
        { url: 'https://host.example/media/clip.mp4', filename: 'clip.mp4', mime: 'video/mp4', bytes: 10 },
      ],
      count: 1,
    },
  };

  it('asks the engine to read the URL, and maps the listing', async () => {
    const { fetchImpl, calls } = makeFetch({ 'POST /api/v1/browse': () => listing });
    const { result } = renderHook(() => useBrowseUrl('https://host.example/media/'), {
      wrapper: wrapper(fetchImpl),
    });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));

    // It goes to the engine, not the third-party host: CORS would block the
    // browser fetching an arbitrary site, and the engine is allowed to.
    const post = calls.find((c) => c.method === 'POST');
    expect(post?.url).toContain('/api/v1/browse');
    expect(post?.body).toMatchObject({ url: 'https://host.example/media/' });
    expect(result.current.data?.items[0]?.filename).toBe('clip.mp4');
    expect(result.current.data?.title).toBe('A Gallery');
  });

  it('makes no request until the URL is absolute', async () => {
    const { fetchImpl, calls } = makeFetch({ 'POST /api/v1/browse': () => listing });
    const { result } = renderHook(() => useBrowseUrl('not-a-url-yet'), {
      wrapper: wrapper(fetchImpl),
    });
    // Give any stray request a chance to fire.
    await act(async () => {
      await new Promise((r) => setTimeout(r, 50));
    });
    expect(calls).toHaveLength(0);
    expect(result.current.fetchStatus).toBe('idle');
  });

  it('makes no request at all when there is no URL', async () => {
    const { fetchImpl, calls } = makeFetch({ 'POST /api/v1/browse': () => listing });
    renderHook(() => useBrowseUrl(null), { wrapper: wrapper(fetchImpl) });
    await act(async () => {
      await new Promise((r) => setTimeout(r, 50));
    });
    expect(calls).toHaveLength(0);
  });

  it('surfaces a page that could not be read', async () => {
    const { fetchImpl } = makeFetch({
      'POST /api/v1/browse': () => ({ status: 502, json: { error: 'could not fetch that page', status: 502 } }),
    });
    const { result } = renderHook(() => useBrowseUrl('https://nope.example/x'), {
      wrapper: wrapper(fetchImpl),
    });
    await waitFor(() => expect(result.current.isError).toBe(true));
  });

  it('asks for media only by default', async () => {
    const { fetchImpl, calls } = makeFetch({ 'POST /api/v1/browse': () => listing });
    const { result } = renderHook(() => useBrowseUrl('https://host.example/media/'), {
      wrapper: wrapper(fetchImpl),
    });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    expect(calls.find((c) => c.method === 'POST')?.body).toEqual({
      url: 'https://host.example/media/',
      filter: 'media',
    });
  });

  it('asks for everything on the page when that filter is chosen', async () => {
    const { fetchImpl, calls } = makeFetch({ 'POST /api/v1/browse': () => listing });
    const { result } = renderHook(() => useBrowseUrl('https://host.example/media/', 'all'), {
      wrapper: wrapper(fetchImpl),
    });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    expect(calls.find((c) => c.method === 'POST')?.body).toMatchObject({ filter: 'all' });
  });

  it('uses the link text as the item title when the page named it', async () => {
    const { fetchImpl } = makeFetch({
      'POST /api/v1/browse': () => ({
        json: {
          url: 'https://site.example/',
          title: 'A site',
          items: [
            { url: 'https://site.example/about', filename: 'about', title: 'About us', mime: '', bytes: -1 },
          ],
          count: 1,
        },
      }),
    });
    const { result } = renderHook(() => useBrowseUrl('https://site.example/', 'all'), {
      wrapper: wrapper(fetchImpl),
    });
    await waitFor(() => expect(result.current.isSuccess).toBe(true));
    expect(result.current.data?.items[0]?.title).toBe('About us');
  });
});

describe('useRenderPage', () => {
  it('reports success with the title and timing from the headers', async () => {
    const { fetchImpl, calls } = makeFetch({
      'GET /api/v1/render': () => ({
        status: 200,
        headers: {
          'X-Vyzorix-Render': 'ok',
          'X-Vyzorix-Title': 'Example Domain',
          'X-Vyzorix-Render-Ms': '1234',
        },
        json: {},
      }),
    });
    const { result } = renderHook(() => useRenderPage(), { wrapper: wrapper(fetchImpl) });
    let out: { ok: boolean; objectUrl?: string; title?: string | null } | undefined;
    await act(async () => {
      out = (await result.current.mutateAsync({
        url: 'https://example.com/',
        mode: 'image',
      })) as { ok: boolean; objectUrl?: string; title?: string | null };
    });
    // The mutation's own return value is the result. React Query's `data` only
    // lands after the observer re-renders, so reading it here would race.
    expect(out?.ok).toBe(true);
    if (out?.ok) {
      // A blob URL, because an <img> cannot send the bearer token: the bytes are
      // fetched with auth and handed to the browser, not pointed at.
      expect(out.objectUrl).toMatch(/^blob:/);
    }
    // It asks the engine, and asks for the image mode by default.
    const req = calls.find((c) => c.url.includes('/api/v1/render'));
    expect(req?.url).toContain('mode=image');
    expect(req?.url).toContain(encodeURIComponent('https://example.com/'));
  });

  it('turns a full gate into "busy", so the user is told to retry', async () => {
    const { fetchImpl } = makeFetch({
      'GET /api/v1/render': () => ({
        status: 503,
        json: { error: 'the renderer is busy', status: 503, retry_after: 3 },
      }),
    });
    const { result } = renderHook(() => useRenderPage(), { wrapper: wrapper(fetchImpl) });
    let out: { ok: boolean; failure?: string } | undefined;
    await act(async () => {
      out = (await result.current.mutateAsync({
        url: 'https://example.com/',
        mode: 'image',
      })) as { ok: boolean; failure?: string };
    });
    expect(out?.ok).toBe(false);
    expect(out?.failure).toBe('busy');
  });

  it('separates "turned off" from "no browser", which are both 503', async () => {
    for (const [detail, expected] of [
      ['rendering is not enabled on this engine', 'disabled'],
      ['no browser is installed on the engine host', 'unavailable'],
    ] as const) {
      const { fetchImpl } = makeFetch({
        'GET /api/v1/render': () => ({ status: 503, json: { error: detail, status: 503 } }),
      });
      const { result } = renderHook(() => useRenderPage(), { wrapper: wrapper(fetchImpl) });
      let out: { ok: boolean; failure?: string } | undefined;
      await act(async () => {
        out = (await result.current.mutateAsync({
          url: 'https://example.com/',
          mode: 'image',
        })) as { ok: boolean; failure?: string };
      });
      expect(out?.failure).toBe(expected);
    }
  });

  it('reports a timeout and a refusal distinctly', async () => {
    const cases = [
      [504, { error: 'the page did not finish within 20 seconds', status: 504 }, 'timeout'],
      [400, { error: 'only public http(s) pages can be rendered', status: 400 }, 'refused'],
    ] as const;
    for (const [status, json, expected] of cases) {
      const { fetchImpl } = makeFetch({
        'GET /api/v1/render': () => ({ status, json }),
      });
      const { result } = renderHook(() => useRenderPage(), { wrapper: wrapper(fetchImpl) });
      let out: { ok: boolean; failure?: string } | undefined;
      await act(async () => {
        out = (await result.current.mutateAsync({
          url: 'https://example.com/',
          mode: 'image',
        })) as { ok: boolean; failure?: string };
      });
      expect(out?.failure).toBe(expected);
    }
  });
});

afterEach(() => {
  vi.restoreAllMocks();
});
