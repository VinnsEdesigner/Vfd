/**
 * Probe + detect resources.
 */

import type { ApiClient } from '../api-client';
import { normalizeMedia, normalizeProbe } from '../domain/normalize';
import type { Media, ProbeResult } from '../domain/media';
import type { WireMedia, WireProbeResult } from '../domain/wire';

export interface MediaResource {
  /** `durationSeconds` lets Vyzor turn manifest BANDWIDTH into real sizes;
   *  without it quality sizes come back unknown rather than guessed. */
  probe(url: string, durationSeconds?: number, signal?: AbortSignal): Promise<ProbeResult>;
  /** Sniff raw bytes (the server accepts an octet-stream body). */
  detect(bytes: ArrayBuffer | Uint8Array, signal?: AbortSignal): Promise<Media>;
}

export function createMediaResource(client: ApiClient): MediaResource {
  return {
    async probe(url, durationSeconds, signal) {
      const wire = await client.transport.request<WireProbeResult>({
        method: 'POST',
        path: '/api/v1/probe',
        body: {
          url,
          ...(durationSeconds !== undefined ? { duration_seconds: durationSeconds } : {}),
        },
        ...(signal ? { signal } : {}),
      });
      return normalizeProbe(wire);
    },

    async detect(bytes, signal) {
      const body = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
      const wire = await client.transport.request<WireMedia>({
        method: 'POST',
        path: '/api/v1/detect',
        rawBody: body as unknown as BodyInit,
        ...(signal ? { signal } : {}),
      });
      const media = normalizeMedia(wire);
      return (
        media ?? {
          kind: 'unknown',
          container: '',
          confidence: 0,
          isVideo: false,
          isImage: false,
          isAudio: false,
          width: 0,
          height: 0,
          qualities: [],
        }
      );
    },
  };
}
