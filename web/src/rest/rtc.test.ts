/**
 * Live-video signalling tests.
 *
 * Vyzor's offer arrives over HTTP, the console answers over HTTP, and input
 * rides a data channel inside the peer connection. What is worth pinning is the
 * part a regression would break quietly: that the offer is answered, that the
 * input channel is created with the label Vyzor expects, and that a message
 * sent before the channel opens is held rather than dropped.
 *
 * Fake RTCPeerConnection and RTCDataChannel are used because jsdom has neither.
 * They are real stand-ins for the browser's own: the code under test uses only
 * `addTransceiver`, `createDataChannel`, `setRemoteDescription`, `createAnswer`,
 * `setLocalDescription` and `close` on the connection, and `send`/`readyState`/
 * `onopen`/`onclose` on the channel.
 */

import { beforeEach, describe, expect, it, vi } from 'vitest';
import { normalizeRtcOffer, normalizeRtcStatus } from '../domain/rtc';
import { LiveVideoSession, type RtcSignalling } from './rtc-session';

class FakeChannel {
  static instances: FakeChannel[] = [];
  readyState = 'connecting';
  sent: string[] = [];
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  label: string;

  constructor(label: string) {
    this.label = label;
    FakeChannel.instances.push(this);
  }
  send(data: string): void {
    this.sent.push(data);
  }
  close(): void {
    this.readyState = 'closed';
    this.onclose?.();
  }
  open(): void {
    this.readyState = 'open';
    this.onopen?.();
  }
}

class FakePC {
  static instances: FakePC[] = [];
  connectionState = 'new';
  ontrack: ((ev: unknown) => void) | null = null;
  onconnectionstatechange: (() => void) | null = null;
  transceivers: Array<{ kind: string; init: unknown }> = [];
  remote: unknown = null;
  closed = false;

  constructor(public config: unknown) {
    FakePC.instances.push(this);
  }
  addTransceiver(kind: string, init: unknown): void {
    this.transceivers.push({ kind, init });
  }
  createDataChannel(label: string): FakeChannel {
    return new FakeChannel(label) as unknown as FakeChannel;
  }
  async setRemoteDescription(d: unknown): Promise<void> {
    this.remote = d;
  }
  async createAnswer(): Promise<{ type: string; sdp: string }> {
    return { type: 'answer', sdp: 'v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n' };
  }
  async setLocalDescription(): Promise<void> {}
  close(): void {
    this.closed = true;
  }
}

function fakeVideo(): HTMLVideoElement {
  return { srcObject: null } as unknown as HTMLVideoElement;
}

/** A signalling stub that records what it was asked to do. */
function fakeSignalling(over: Partial<RtcSignalling> = {}): RtcSignalling & {
  answered: Array<{ token: string; sdp: string }>;
  closed: string[];
} {
  const answered: Array<{ token: string; sdp: string }> = [];
  const closed: string[] = [];
  return {
    answered,
    closed,
    async open() {
      return { token: 'tok123', sdp: 'v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n', width: 1280, height: 900 };
    },
    async answer(token, sdp) {
      answered.push({ token, sdp });
    },
    async close(token) {
      closed.push(token);
    },
    ...over,
  };
}

beforeEach(() => {
  FakePC.instances = [];
  FakeChannel.instances = [];
  (globalThis as unknown as { RTCPeerConnection: unknown }).RTCPeerConnection = FakePC;
});

describe('normalizeRtcOffer', () => {
  it('reads a token, an sdp and a viewport', () => {
    expect(normalizeRtcOffer({ token: 'abc', sdp: 'v=0', width: 800, height: 600 })).toEqual({
      token: 'abc',
      sdp: 'v=0',
      viewport: { width: 800, height: 600 },
    });
  });

  it('refuses an offer with no token or no sdp', () => {
    expect(() => normalizeRtcOffer({ sdp: 'v=0' })).toThrow(/token/);
    expect(() => normalizeRtcOffer({ token: 'abc' })).toThrow(/offer/);
  });
});

describe('normalizeRtcStatus', () => {
  it('reads a state and tolerates an unknown one', () => {
    expect(normalizeRtcStatus({ state: 'connected', connected: true })).toEqual({
      state: 'connected',
      connected: true,
      reason: '',
    });
    expect(normalizeRtcStatus({ state: 'exploded' }).state).toBe('new');
    expect(normalizeRtcStatus({}).state).toBe('new');
  });
});

describe('LiveVideoSession', () => {
  it('answers the offer, recvonly, and sends the answer', async () => {
    const signalling = fakeSignalling();
    const s = new LiveVideoSession(fakeVideo(), signalling);
    await s.connect('https://x/');

    expect(signalling.answered).toHaveLength(1);
    expect(signalling.answered[0]!.token).toBe('tok123');
    expect(signalling.answered[0]!.sdp).toContain('m=video');

    const pc = FakePC.instances[0]!;
    expect(pc.transceivers).toEqual([{ kind: 'video', init: { direction: 'recvonly' } }]);
  });

  it('creates the input channel with the label Vyzor looks for', async () => {
    const s = new LiveVideoSession(fakeVideo(), fakeSignalling());
    await s.connect('https://x/');
    expect(FakeChannel.instances).toHaveLength(1);
    expect(FakeChannel.instances[0]!.label).toBe('input');
  });

  it('reports the viewport from the offer', async () => {
    const onOffer = vi.fn();
    const s = new LiveVideoSession(fakeVideo(), fakeSignalling(), { onOffer });
    await s.connect('https://x/');
    expect(onOffer).toHaveBeenCalledWith({ width: 1280, height: 900 });
  });

  it('holds input sent before the channel opens, then flushes it', async () => {
    const s = new LiveVideoSession(fakeVideo(), fakeSignalling());
    await s.connect('https://x/');
    const ch = FakeChannel.instances[0]!;

    // Sent while the channel is still connecting.
    s.send({ type: 'mouse', event: 'mousePressed' });
    expect(ch.sent).toEqual([]);

    ch.open();
    expect(ch.sent).toEqual([JSON.stringify({ type: 'mouse', event: 'mousePressed' })]);
  });

  it('sends immediately once the channel is open', async () => {
    const s = new LiveVideoSession(fakeVideo(), fakeSignalling());
    await s.connect('https://x/');
    const ch = FakeChannel.instances[0]!;
    ch.open();

    s.send({ type: 'navigate', url: 'https://example.org/' });
    expect(ch.sent).toEqual([JSON.stringify({ type: 'navigate', url: 'https://example.org/' })]);
  });

  it('reports live when media arrives', async () => {
    const onLive = vi.fn();
    const video = fakeVideo();
    const s = new LiveVideoSession(video, fakeSignalling(), { onLive });
    await s.connect('https://x/');

    const track = { kind: 'video' };
    FakePC.instances[0]!.ontrack!({ track, streams: [{}] });
    expect(onLive).toHaveBeenCalled();
  });

  it('reports the connection state', async () => {
    const onState = vi.fn();
    const s = new LiveVideoSession(fakeVideo(), fakeSignalling(), { onState });
    await s.connect('https://x/');

    const pc = FakePC.instances[0]!;
    pc.connectionState = 'connected';
    pc.onconnectionstatechange!();
    expect(onState).toHaveBeenLastCalledWith('connected', true);
    expect(s.isConnected()).toBe(true);
  });

  it('names a failed connection and ends once', async () => {
    const onEnded = vi.fn();
    const s = new LiveVideoSession(fakeVideo(), fakeSignalling(), { onEnded });
    await s.connect('https://x/');

    const pc = FakePC.instances[0]!;
    pc.connectionState = 'failed';
    pc.onconnectionstatechange!();
    pc.onconnectionstatechange!();
    expect(onEnded).toHaveBeenCalledTimes(1);
    expect(onEnded).toHaveBeenCalledWith('the video connection failed');
  });

  it('reports a failed open rather than throwing', async () => {
    const onError = vi.fn();
    const onEnded = vi.fn();
    const signalling = fakeSignalling({
      open: async () => {
        throw new Error('Vyzor is busy');
      },
    });
    const s = new LiveVideoSession(fakeVideo(), signalling, { onError, onEnded });
    await s.connect('https://x/');
    expect(onError).toHaveBeenCalledWith('Vyzor is busy');
    expect(onEnded).toHaveBeenCalledWith('Vyzor is busy');
  });

  it('tells Vyzor to close, and detaches the video', async () => {
    const signalling = fakeSignalling();
    const video = fakeVideo();
    const s = new LiveVideoSession(video, signalling);
    await s.connect('https://x/');

    s.close();
    expect(signalling.closed).toEqual(['tok123']);
    expect(FakePC.instances[0]!.closed).toBe(true);
    expect(video.srcObject).toBeNull();
  });

  it('does not send input after close', async () => {
    const s = new LiveVideoSession(fakeVideo(), fakeSignalling());
    await s.connect('https://x/');
    const ch = FakeChannel.instances[0]!;
    ch.open();
    s.close();
    s.send({ type: 'reload' });
    expect(ch.sent).toEqual([]);
  });
});
