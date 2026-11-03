/**
 * HTTP transport. Knows how to send a request and decode JSON; knows nothing
 * about jobs, media or the domain beyond the error shape.
 *
 * Swappable: anything that satisfies `Transport` can back the rest layer
 * (fetch today, a mock or a worker tomorrow).
 */

import { ApiError, NetworkError } from './errors';

export interface RequestOptions {
  method?: 'GET' | 'POST' | 'PATCH' | 'DELETE' | 'PUT' | 'OPTIONS';
  path: string;
  query?: Record<string, string | number | boolean | undefined>;
  body?: unknown;
  /** Raw body (used for binary detect uploads). */
  rawBody?: BodyInit;
  headers?: Record<string, string>;
  signal?: AbortSignal;
}

export interface Transport {
  request<T>(options: RequestOptions): Promise<T>;
  /** Resolve a path to an absolute URL (for links shown in the UI). */
  resolve(path: string): string;
}

export interface TransportConfig {
  baseUrl: string;
  /** Optional bearer token appended to every request. */
  authToken?: string | undefined;
  /** Extra headers (e.g. storage credentials). */
  defaultHeaders?: Record<string, string>;
  /** Injectable for tests. */
  fetchImpl?: typeof fetch;
}

function buildQuery(query: RequestOptions['query']): string {
  if (!query) return '';
  const params = new URLSearchParams();
  for (const [k, v] of Object.entries(query)) {
    if (v === undefined) continue;
    params.set(k, String(v));
  }
  const s = params.toString();
  return s ? `?${s}` : '';
}

export function createTransport(config: TransportConfig): Transport {
  const fetchImpl = config.fetchImpl ?? globalThis.fetch;
  const base = config.baseUrl.replace(/\/+$/, '');

  async function parseError(res: Response): Promise<never> {
    let detail: string | null = null;
    try {
      const text = await res.text();
      if (text) {
        try {
          const parsed = JSON.parse(text) as { error?: string };
          detail = parsed.error ?? text;
        } catch {
          detail = text;
        }
      }
    } catch {
      detail = null;
    }
    throw new ApiError(detail ?? `request failed with status ${res.status}`, res.status, detail);
  }

  return {
    resolve(path: string): string {
      return `${base}${path.startsWith('/') ? path : `/${path}`}`;
    },

    async request<T>(options: RequestOptions): Promise<T> {
      const url = `${base}${options.path}${buildQuery(options.query)}`;
      const headers: Record<string, string> = { ...config.defaultHeaders, ...options.headers };
      if (config.authToken) headers['Authorization'] = `Bearer ${config.authToken}`;

      let body: BodyInit | undefined;
      if (options.rawBody !== undefined) {
        body = options.rawBody;
        headers['Content-Type'] = headers['Content-Type'] ?? 'application/octet-stream';
      } else if (options.body !== undefined) {
        headers['Content-Type'] = headers['Content-Type'] ?? 'application/json';
        body = JSON.stringify(options.body);
      }

      let res: Response;
      try {
        res = await fetchImpl(url, {
          method: options.method ?? 'GET',
          headers,
          body,
          signal: options.signal,
        });
      } catch (cause) {
        if (cause instanceof DOMException && cause.name === 'AbortError') throw cause;
        throw new NetworkError(`network request to ${options.path} failed`, cause);
      }

      if (!res.ok) await parseError(res);
      if (res.status === 204) return undefined as T;

      const text = await res.text();
      if (!text) return undefined as T;
      return JSON.parse(text) as T;
    },
  };
}
