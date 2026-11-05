/**
 * Live video over WebRTC, as the console sees it.
 *
 * Vyzor captures the page as raw video and sends it as SRTP; the console
 * plays it in a <video>. The signalling is not WebRTC's own: WebRTC has none by
 * design, so it rides a few plain HTTP requests, and input rides a data channel
 * inside the same peer connection.
 *
 * The types here are the console's own, not the wire's. `domain/wire.ts` holds
 * the shapes as they arrive and this module is the one place that reads them, so
 * a wire change stops at the boundary.
 */

import type { WireRtcOffer, WireRtcStatus } from './wire';

/** The connection's own state, as the browser reports it. */
export type LiveVideoState =
  | 'new'
  | 'connecting'
  | 'connected'
  | 'disconnected'
  | 'failed'
  | 'closed';

/** The viewport Vyzor is capturing, so the view can keep the page's shape. */
export interface LiveVideoViewport {
  width: number;
  height: number;
}

/** Vyzor's answer to `POST /api/v1/rtc`. */
export interface LiveVideoOffer {
  token: string;
  sdp: string;
  viewport: LiveVideoViewport;
}

/**
 * Read Vyzor's offer.
 *
 * Throws on anything unusable — no token, no SDP — because a session without
 * either cannot be established, and a caller that carries on would build a peer
 * connection nothing answers.
 */
export function normalizeRtcOffer(w: WireRtcOffer): LiveVideoOffer {
  if (!w || typeof w !== 'object') throw new Error('Vyzor sent no offer');
  const token = typeof w.token === 'string' ? w.token : '';
  const sdp = typeof w.sdp === 'string' ? w.sdp : '';
  if (!token) throw new Error('Vyzor sent no session token');
  if (!sdp) throw new Error('Vyzor sent an empty offer');
  return {
    token,
    sdp,
    viewport: { width: w.width ?? 0, height: w.height ?? 0 },
  };
}

/** The states Vyzor is allowed to name; anything else reads as new. */
const STATES: ReadonlyArray<LiveVideoState> = [
  'new',
  'connecting',
  'connected',
  'disconnected',
  'failed',
  'closed',
];

/** Read a status poll. Tolerant: a poll is only ever used for display. */
export function normalizeRtcStatus(w: WireRtcStatus): {
  state: LiveVideoState;
  connected: boolean;
  reason: string;
} {
  const state = STATES.includes(w?.state as LiveVideoState)
    ? (w.state as LiveVideoState)
    : 'new';
  return { state, connected: w?.connected === true, reason: w?.reason ?? '' };
}
