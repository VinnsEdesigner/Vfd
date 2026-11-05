/**
 * A live video session: an RTCPeerConnection, signalled over plain HTTP.
 *
 * NO WEBSOCKET, AND WHERE INPUT GOES INSTEAD
 *
 * WebRTC cannot bootstrap itself — the offer and answer have to cross something
 * before a peer connection exists — so *something* has to carry them. Here that
 * something is a handful of HTTP requests, not a long-lived socket:
 *
 *   POST   /api/v1/rtc                -> { token, sdp, viewport }   (the offer)
 *   POST   /api/v1/rtc/{token}/answer { sdp }                       (the answer)
 *   GET    /api/v1/rtc/{token}        -> { state, connected }        (poll)
 *   DELETE /api/v1/rtc/{token}                                       (close)
 *
 * Input does not need a socket either: it rides a WebRTC data channel, inside the
 * same peer connection the video already uses. So the whole session — media and
 * control — is one WebRTC connection, and HTTP only establishes it.
 *
 * Because there is no socket to trickle ICE on, Vyzor returns the offer only
 * once its gathering has finished, so the candidates are already in the SDP.
 *
 * WHAT THIS OWNS
 *
 * The peer connection, the data channel, and the media stream. The <video>
 * element is handed in, because the element belongs to the view that displays it;
 * this only ever assigns `srcObject` on it.
 */

import { normalizeRtcOffer, type LiveVideoState, type LiveVideoViewport } from '../domain/rtc';

/** The label of the data channel that carries input. Vyzor looks for this
 *  exact name, so it is a contract, not a preference. */
const INPUT_LABEL = 'input';

/**
 * The STUN servers a browser uses to discover its own address.
 *
 * A stream between two hosts on the same network needs none, but one across a
 * NAT does — Vyzor's host candidates alone are not enough then. Google's is
 * the conventional public one and needs no account.
 */
const DEFAULT_ICE: RTCConfiguration = {
  iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
};

export interface LiveVideoHandlers {
  /** The offer arrived: Vyzor's capture size is now known. */
  onOffer?: (viewport: LiveVideoViewport) => void;
  /** The connection's state changed. */
  onState?: (state: LiveVideoState, connected: boolean) => void;
  /** Media is flowing: the stream is attached and playable. */
  onLive?: (stream: MediaStream) => void;
  /** A signalling or transport error worth showing. */
  onError?: (message: string) => void;
  /** The session ended, with a reason fit to show a user. */
  onEnded?: (reason: string) => void;
}

/**
 * How the console talks to Vyzor's signalling.
 *
 * Supplied by the rest layer, so this class holds no knowledge of the API's
 * shape — and so a test can drive it without a server.
 */
export interface RtcSignalling {
  open(url: string): Promise<{ token: string; sdp: string; width: number; height: number }>;
  answer(token: string, sdp: string): Promise<void>;
  close(token: string): Promise<void>;
}

export class LiveVideoSession {
  private pc: RTCPeerConnection | null = null;
  /** The channel input is sent on. Created before the answer, so it is part of
   *  the negotiation rather than a second one. */
  private channel: RTCDataChannel | null = null;
  private token = '';
  private state: LiveVideoState = 'new';
  private ended = false;
  /** Input sent before the channel opens, so a fast user's first click is not
   *  silently dropped. Bounded: a queue that grows is a leak, and stale input is
   *  worse than none. */
  private pending: string[] = [];

  constructor(
    private readonly video: HTMLVideoElement,
    private readonly signalling: RtcSignalling,
    private readonly handlers: LiveVideoHandlers = {},
    private readonly iceConfig: RTCConfiguration = DEFAULT_ICE,
  ) {}

  getState(): LiveVideoState {
    return this.state;
  }

  isConnected(): boolean {
    return this.state === 'connected';
  }

  /** Open the session: fetch the offer, answer it, and start the media. */
  async connect(url: string): Promise<void> {
    let offer: { token: string; sdp: string; width: number; height: number };
    try {
      offer = await this.signalling.open(url);
    } catch (e) {
      this.fail(e instanceof Error ? e.message : 'could not open a session');
      return;
    }
    if (this.ended) return; // closed while the offer was in flight

    this.token = offer.token;
    this.handlers.onOffer?.({ width: offer.width, height: offer.height });
    await this.applyOffer(offer.sdp);
  }

  /**
   * Take Vyzor's offer, produce an answer, and send it back.
   *
   * The video transceiver is created `recvonly`: the console only ever receives
   * media. Without it the browser would offer to send video back, and Vyzor
   * would have to negotiate a stream nothing produces.
   */
  private async applyOffer(sdp: string): Promise<void> {
    if (!sdp) {
      this.fail('Vyzor sent an empty offer');
      return;
    }
    let pc: RTCPeerConnection;
    try {
      pc = new RTCPeerConnection(this.iceConfig);
    } catch {
      this.fail('this browser cannot do WebRTC');
      return;
    }
    this.pc = pc;

    pc.addTransceiver('video', { direction: 'recvonly' });

    /* The input channel, made here so it is part of the offer/answer rather than
     * needing a second negotiation. Vyzor finds it by its label. */
    try {
      this.channel = pc.createDataChannel(INPUT_LABEL, { ordered: true });
      this.wireChannel(this.channel);
    } catch {
      /* A browser without data channels still gets video; only input is lost. */
    }

    pc.ontrack = (ev: RTCTrackEvent) => {
      const stream = ev.streams[0] ?? new MediaStream([ev.track]);
      try {
        this.video.srcObject = stream;
      } catch {
        /* a detached element; nothing to attach to */
      }
      this.handlers.onLive?.(stream);
    };

    pc.onconnectionstatechange = () => {
      this.setState(pc.connectionState as LiveVideoState, pc.connectionState === 'connected');
      if (pc.connectionState === 'failed') this.finish('the video connection failed');
    };

    try {
      await pc.setRemoteDescription({ type: 'offer', sdp });
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      /* No trickle: the answer is sent once. Vyzor sets it as the remote
       * description rather than adding candidates one at a time, so what it needs
       * is in the document. */
      await this.signalling.answer(this.token, answer.sdp ?? '');
    } catch (e) {
      this.fail(e instanceof Error ? e.message : 'Vyzor’s offer could not be answered');
    }
  }

  private wireChannel(ch: RTCDataChannel): void {
    ch.onopen = () => {
      for (const msg of this.pending) ch.send(msg);
      this.pending = [];
    };
    ch.onclose = () => {
      this.channel = null;
    };
  }

  /** Send one input command. Queued briefly if the channel is not open yet. */
  send(message: unknown): void {
    if (this.ended) return;
    const json = JSON.stringify(message);
    const ch = this.channel;
    if (ch && ch.readyState === 'open') {
      ch.send(json);
      return;
    }
    /* Held, not dropped: a click made the instant the page appears is real
     * input. Bounded so a dead channel cannot grow without limit. */
    if (this.pending.length < 32) this.pending.push(json);
  }

  private setState(state: LiveVideoState, connected: boolean): void {
    this.state = state;
    this.handlers.onState?.(state, connected);
  }

  private fail(message: string): void {
    this.handlers.onError?.(message);
    this.finish(message);
  }

  private finish(reason: string): void {
    if (this.ended) return;
    this.ended = true;
    this.setState('closed', false);
    this.handlers.onEnded?.(reason);
  }

  /** Close deliberately. Idempotent. */
  close(): void {
    if (this.ended) return;
    this.ended = true;
    this.setState('closed', false);
    const token = this.token;
    this.token = '';
    /* Vyzor frees the browser, the display and the pipeline on this call, so
     * it matters even though the peer connection is closing anyway. */
    if (token) void this.signalling.close(token).catch(() => undefined);
    try {
      this.channel?.close();
    } catch {
      /* already closing */
    }
    this.channel = null;
    this.pc?.close();
    this.pc = null;
    if (this.video) this.video.srcObject = null;
  }
}

/** Re-exported so a caller can read an offer without importing the domain module
 *  directly. */
export { normalizeRtcOffer };
