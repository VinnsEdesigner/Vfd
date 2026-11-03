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

import { MediaApiProvider, useDownloads, useCreateDownload, useHealth } from '../src/hooks';
import { useConnectionStore, connectionSettings } from '../src/stores/connection-store';
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
    const result = handler(init) as { status?: number; json: unknown };
    const status = result.status ?? 200;
    return new Response(JSON.stringify(result.json), {
      status,
      headers: { 'Content-Type': 'application/json' },
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
    expect(useConnectionStore.getState().baseUrl).toBe('http://127.0.0.1:8787');
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

afterEach(() => {
  vi.restoreAllMocks();
});
