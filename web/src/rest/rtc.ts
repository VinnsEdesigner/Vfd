/**
 * Where a live video session is opened and signalled.
 *
 * Four plain HTTP calls, no WebSocket: WebRTC cannot carry its own bootstrap, so
 * the offer and answer cross HTTP, and everything after that — video and input —
 * is one WebRTC connection. See `rtc-session.ts` for the full picture.
 *
 * The session token is the credential for the follow-up calls, which is why the
 * Vyzor mints it and the console echoes it.
 */

import type { ApiClient } from '../api-client';
import { normalizeRtcOffer, normalizeRtcStatus } from '../domain/rtc';
import type { WireRtcOffer, WireRtcStatus } from '../domain/wire';
import { LiveVideoSession, type LiveVideoHandlers, type RtcSignalling } from './rtc-session';

export function createRtcResource(client: ApiClient) {
  /**
   * The signalling, over the same authenticated transport as every other call.
   *
   * Going through the transport matters: it carries Vyzor's bearer token, so
   * a secured Vyzor works the same as an open one, and a failure comes back as a
   * typed `ApiError` with Vyzor's own message rather than an opaque status.
   */
  const signalling: RtcSignalling = {
    async open(url: string) {
      const wire = await client.transport.request<WireRtcOffer>({
        method: 'POST',
        path: '/api/v1/rtc',
        body: { url },
      });
      const offer = normalizeRtcOffer(wire);
      return {
        token: offer.token,
        sdp: offer.sdp,
        width: offer.viewport.width,
        height: offer.viewport.height,
      };
    },

    async answer(token: string, sdp: string) {
      await client.transport.request<{ ok: boolean }>({
        method: 'POST',
        path: `/api/v1/rtc/${encodeURIComponent(token)}/answer`,
        body: { sdp },
      });
    },

    async close(token: string) {
      await client.transport.request<{ ok: boolean }>({
        method: 'DELETE',
        path: `/api/v1/rtc/${encodeURIComponent(token)}`,
      });
    },
  };

  return {
    /**
     * Open a session and wire it to a video element.
     *
     * The element is passed in rather than returned, because it belongs to the
     * view that displays it; the session only ever assigns `srcObject` on it.
     */
    open(url: string, video: HTMLVideoElement, handlers: LiveVideoHandlers = {}): LiveVideoSession {
      return new LiveVideoSession(video, signalling, handlers);
    },

    /** Poll a session's state, for a caller that is not holding the handle. */
    async status(token: string) {
      const wire = await client.transport.request<WireRtcStatus>({
        method: 'GET',
        path: `/api/v1/rtc/${encodeURIComponent(token)}`,
      });
      return normalizeRtcStatus(wire);
    },
  };
}

export type RtcResource = ReturnType<typeof createRtcResource>;
export type { RtcSignalling, LiveVideoHandlers } from './rtc-session';
