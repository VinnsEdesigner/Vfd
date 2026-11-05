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

/** Response headers, lower-cased, from a request whose body is not JSON. */
export type ResponseHeaders = Record<string, string>;

export interface Transport {
  request<T>(options: RequestOptions): Promise<T>;
  /**
   * Send a request and return only its headers.
   *
   * For the cases where the *response metadata* is the answer and the body is
   * either huge or not JSON — asking Vyzor whether a page may be framed,
   * for instance. The body is read and discarded so the connection is released.
   */
  head(options: RequestOptions): Promise<ResponseHeaders>;
  /**
   * Fetch bytes *and* headers together, with the same auth as any other request.
   *
   * Needed because an `<img>` or `<iframe>` sends no Authorization header, so a
   * protected resource cannot simply be pointed at — the console fetches it and
   * hands the browser a blob instead. Without this, a render would work on an
   * open Vyzor and 401 on a secured one, which is the worst kind of difference:
   * it only shows up once someone turns auth on.
   *
   * The headers come back too, because for these endpoints the metadata *is* the
   * answer — whether a render succeeded, and why it did not.
   */
  fetchWithHeaders(options: RequestOptions): Promise<{ blob: Blob; headers: ResponseHeaders }>;
  /** Resolve a path to an absolute URL (for links shown in the UI). */
  resolve(path: string): string;
  /**
   * Resolve a path to an absolute `ws:`/`wss:` URL, carrying the bearer token
   * as a query parameter.
   *
   * A browser's WebSocket API cannot set an Authorization header, so the token
   * has to travel in the URL. That is Vyzor's own design (it accepts a
   * `token` query parameter for exactly this endpoint), and it is why Vyzor
   * also checks the Origin: a WebSocket handshake is not subject to CORS.
   */
  socketUrl(path: string, query?: Record<string, string | number>): string;
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

    socketUrl(path: string, query?: Record<string, string | number>): string {
      const httpUrl = `${base}${path.startsWith('/') ? path : `/${path}`}`;
      // A base URL may be http or https; a socket must be ws or wss.
      const wsUrl = httpUrl.replace(/^http:/, 'ws:').replace(/^https:/, 'wss:');
      const params = new URLSearchParams();
      if (query) {
        for (const [k, v] of Object.entries(query)) params.set(k, String(v));
      }
      if (config.authToken) params.set('token', config.authToken);
      const qs = params.toString();
      return qs ? `${wsUrl}?${qs}` : wsUrl;
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
      /*
       * A 200 that is not JSON is not a valid answer, and saying so is worth
       * more than the SyntaxError JSON.parse would throw.
       *
       * This happens for a real reason: a static host serves its own SPA for
       * any unknown path, so pointing the console at its own origin returns
       * `200 text/html` for `/api/v1/...`. The request looks successful and
       * then dies in the parser, which reads as "the console is broken" when
       * the truth is "that address is not Vyzor".
       */
      try {
        return JSON.parse(text) as T;
      } catch {
        throw new ApiError(
          'the response was not JSON',
          0,
          `${options.path} answered ${res.status} ${res.headers.get('content-type') ?? 'with no content type'} — this address is not Vyzor`,
        );
      }
    },

    async head(options: RequestOptions): Promise<ResponseHeaders> {
      const url = `${base}${options.path}${buildQuery(options.query)}`;
      const headers: Record<string, string> = { ...config.defaultHeaders, ...options.headers };
      if (config.authToken) headers['Authorization'] = `Bearer ${config.authToken}`;

      let res: Response;
      try {
        res = await fetchImpl(url, {
          method: options.method ?? 'GET',
          headers,
          signal: options.signal,
        });
      } catch (cause) {
        if (cause instanceof DOMException && cause.name === 'AbortError') throw cause;
        throw new NetworkError(`network request to ${options.path} failed`, cause);
      }

      if (!res.ok) await parseError(res);

      const out: ResponseHeaders = {};
      res.headers.forEach((value, key) => {
        out[key.toLowerCase()] = value;
      });
      // Read and discard the body, so the connection is not left dangling. A
      // body that is huge or not text must not be parsed here.
      await res.arrayBuffer().catch(() => undefined);
      return out;
    },

    async fetchWithHeaders(
      options: RequestOptions,
    ): Promise<{ blob: Blob; headers: ResponseHeaders }> {
      const url = `${base}${options.path}${buildQuery(options.query)}`;
      const headers: Record<string, string> = { ...config.defaultHeaders, ...options.headers };
      if (config.authToken) headers['Authorization'] = `Bearer ${config.authToken}`;

      let res: Response;
      try {
        res = await fetchImpl(url, {
          method: options.method ?? 'GET',
          headers,
          signal: options.signal,
        });
      } catch (cause) {
        if (cause instanceof DOMException && cause.name === 'AbortError') throw cause;
        throw new NetworkError(`network request to ${options.path} failed`, cause);
      }
      if (!res.ok) await parseError(res);

      const out: ResponseHeaders = {};
      res.headers.forEach((value, key) => {
        out[key.toLowerCase()] = value;
      });
      return { blob: await res.blob(), headers: out };
    },
  };
}
