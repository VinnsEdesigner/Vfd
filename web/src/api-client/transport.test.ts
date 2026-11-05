/**
 * The transport's handling of an answer that is not JSON.
 *
 * This is not hypothetical: a static host (Cloudflare Pages, Netlify, any SPA
 * host) answers *every* path with its own `index.html` and a 200. So a console
 * pointed at its own origin gets `200 text/html` for `/api/v1/capabilities`,
 * the request looks successful, and it used to die in `JSON.parse` with a bare
 * `SyntaxError` — which reads as "the console is broken" rather than "that
 * address is not Vyzor".
 *
 * The transport now names the real problem. These pin that, and the behaviour
 * it must not regress: a genuine error response is still reported as one.
 */

import { describe, expect, it, vi } from 'vitest';
import { createApiClient } from './index';
import { ApiError } from './errors';

function clientReturning(response: Response) {
  const fetchImpl = vi.fn(async () => response);
  return {
    api: createApiClient({ baseUrl: 'https://not-vyzor.example', fetchImpl: fetchImpl as unknown as typeof fetch }),
    fetchImpl,
  };
}

describe('transport: a 200 that is not JSON', () => {
  it('reports it as an ApiError rather than a raw SyntaxError', async () => {
    const { api } = clientReturning(
      new Response('<!doctype html>\n<html>…', {
        status: 200,
        headers: { 'content-type': 'text/html; charset=utf-8' },
      }),
    );
    await expect(api.transport.request({ method: 'GET', path: '/api/v1/capabilities' })).rejects.toBeInstanceOf(
      ApiError,
    );
  });

  it('says the address is not Vyzor, and what it answered with', async () => {
    const { api } = clientReturning(
      new Response('<!doctype html>', {
        status: 200,
        headers: { 'content-type': 'text/html; charset=utf-8' },
      }),
    );
    const err = (await api.transport
      .request({ method: 'GET', path: '/api/v1/capabilities' })
      .catch((e: unknown) => e)) as ApiError;
    expect(err.status).toBe(0);
    expect(err.detail).toMatch(/not Vyzor/i);
    expect(err.detail).toMatch(/text\/html/);
  });

  it('still parses a real JSON answer', async () => {
    const { api } = clientReturning(
      new Response(JSON.stringify({ ok: true }), {
        status: 200,
        headers: { 'content-type': 'application/json' },
      }),
    );
    await expect(
      api.transport.request({ method: 'GET', path: '/api/v1/capabilities' }),
    ).resolves.toEqual({ ok: true });
  });

  it('still reports a genuine error response as its own status', async () => {
    const { api } = clientReturning(
      new Response(JSON.stringify({ detail: 'no such thing' }), {
        status: 404,
        headers: { 'content-type': 'application/json' },
      }),
    );
    const err = (await api.transport
      .request({ method: 'GET', path: '/api/v1/downloads/nope' })
      .catch((e: unknown) => e)) as ApiError;
    expect(err.status).toBe(404);
  });
});
