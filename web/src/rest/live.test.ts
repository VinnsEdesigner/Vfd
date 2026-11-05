/**
 * Live-session socket tests.
 *
 * Vyzor sends two kinds of message over one socket: JSON text (ready, state,
 * closed, error, and a header for each image) and binary (the image itself). The
 * decoder's job is to pair a header with the binary that follows it and hand the
 * caller one `LiveFrame`, and to revoke nothing it does not own.
 *
 * A fake WebSocket is used because jsdom has none. It is a real stand-in for the
 * browser's own: the code under test uses only `onopen`, `onmessage`, `onclose`,
 * `onerror`, `send`, `close` and `readyState`, which is the whole surface the
 * real object exposes for this use.
 */

import { beforeEach, describe, expect, it, vi } from 'vitest';
import { createApiClient } from '../api-client';
import { createLiveResource, LiveSession } from './browse';
import type { LiveFrame, LiveMessage } from '../domain/browse';

/** A WebSocket good enough for the decoder, driven by hand from the test. */
class FakeSocket {
  static instances: FakeSocket[] = [];
  static readonly OPEN = 1;
  static readonly CLOSED = 3;

  readyState = 0;
  binaryType = 'blob';
  sent: string[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onclose: ((ev: { wasClean: boolean }) => void) | null = null;
  onerror: (() => void) | null = null;
  url: string;

  constructor(url: string) {
    this.url = url;
    FakeSocket.instances.push(this);
  }

  send(data: string): void {
    this.sent.push(data);
  }

  close(): void {
    this.readyState = FakeSocket.CLOSED;
  }

  // --- test drivers ---
  open(): void {
    this.readyState = FakeSocket.OPEN;
    this.onopen?.();
  }
  text(obj: unknown): void {
    this.onmessage?.({ data: JSON.stringify(obj) });
  }
  binary(bytes: Uint8Array): void {
    this.onmessage?.({ data: bytes.buffer });
  }
  drop(clean = false): void {
    this.onclose?.({ wasClean: clean });
  }
}

beforeEach(() => {
  FakeSocket.instances = [];
  (globalThis as unknown as { WebSocket: unknown }).WebSocket = FakeSocket as unknown as typeof WebSocket;
});

function client() {
  return createApiClient({ baseUrl: 'http://Vyzor.test:8787' });
}

describe('createLiveResource', () => {
  it('builds a ws:// url with the query and, when set, the token', () => {
    const withToken = createApiClient({ baseUrl: 'http://Vyzor.test:8787', authToken: 'secret' });
    const url = createLiveResource(withToken).url('https://example.com/', 800, 600);
    const parsed = new URL(url);
    expect(parsed.protocol).toBe('ws:');
    expect(parsed.pathname).toBe('/api/v1/interactive');
    expect(parsed.searchParams.get('url')).toBe('https://example.com/');
    expect(parsed.searchParams.get('w')).toBe('800');
    expect(parsed.searchParams.get('h')).toBe('600');
    expect(parsed.searchParams.get('token')).toBe('secret');
  });

  it('uses wss: for an https Vyzor and omits the token when there is none', () => {
    const secure = createApiClient({ baseUrl: 'https://Vyzor.example' });
    const url = createLiveResource(secure).url('https://example.com/', 1024, 768);
    expect(new URL(url).protocol).toBe('wss:');
    expect(new URL(url).searchParams.has('token')).toBe(false);
  });

  it('opens a session and connects it', () => {
    const session = createLiveResource(client()).open('https://example.com/', 800, 600, {});
    session.connect();
    expect(FakeSocket.instances).toHaveLength(1);
  });
});

describe('LiveSession decoding', () => {
  function sessionWith(handlers: {
    onMessage?: (m: LiveMessage) => void;
    onFrame?: (f: LiveFrame) => void;
    onOpen?: () => void;
    onClose?: (f: string, r: string) => void;
  }) {
    const session = new LiveSession('ws://Vyzor.test/api/v1/interactive?url=x', handlers);
    session.connect();
    return { session, socket: FakeSocket.instances[0]! };
  }

  it('reports open, ready and state', () => {
    const onMessage = vi.fn();
    const onOpen = vi.fn();
    const { socket } = sessionWith({ onMessage, onOpen });
    socket.open();
    expect(onOpen).toHaveBeenCalled();
    socket.text({ type: 'ready', url: 'https://example.com/', width: 800, height: 600 });
    socket.text({ type: 'state', page: { url: 'https://example.com/', title: 'Example', loading: false } });
    expect(onMessage).toHaveBeenCalledTimes(2);
    expect(onMessage).toHaveBeenLastCalledWith({
      kind: 'state',
      page: { url: 'https://example.com/', title: 'Example', loading: false, editing: false },
    });
  });

  it('pairs a frame header with the binary that follows', () => {
    const onFrame = vi.fn();
    const { socket } = sessionWith({ onFrame });
    socket.open();
    socket.text({ type: 'frame', format: 'jpeg', width: 800, height: 600, seq: 7 });
    // The header alone yields no frame: the image has not arrived.
    expect(onFrame).not.toHaveBeenCalled();
    socket.binary(new Uint8Array([0xff, 0xd8, 0xff]));
    expect(onFrame).toHaveBeenCalledTimes(1);
    const frame = onFrame.mock.calls[0]![0] as LiveFrame;
    expect(frame.still).toBe(false);
    expect(frame.width).toBe(800);
    expect(frame.height).toBe(600);
    expect(frame.seq).toBe(7);
    expect(frame.url.startsWith('blob:')).toBe(true);
  });

  it('marks a screenshot as a still', () => {
    const onFrame = vi.fn();
    const { socket } = sessionWith({ onFrame });
    socket.open();
    socket.text({ type: 'shot', format: 'png', width: 800, height: 600, seq: 9 });
    socket.binary(new Uint8Array([0x89, 0x50, 0x4e, 0x47]));
    expect((onFrame.mock.calls[0]![0] as LiveFrame).still).toBe(true);
  });

  it('sends a command as JSON', () => {
    const { session, socket } = sessionWith({});
    socket.open();
    session.send({ type: 'navigate', url: 'https://example.org/' });
    expect(socket.sent).toEqual([JSON.stringify({ type: 'navigate', url: 'https://example.org/' })]);
  });

  it('does not send on a socket that is not open', () => {
    const { session, socket } = sessionWith({});
    // Not opened.
    session.send({ type: 'reload' });
    expect(socket.sent).toEqual([]);
  });

  it('reports a deliberate close as closed', () => {
    const onClose = vi.fn();
    const { socket } = sessionWith({ onClose });
    socket.open();
    socket.drop(true);
    expect(onClose).toHaveBeenCalledWith('closed', expect.any(String));
  });

  it('reports a dropped connection as a failure', () => {
    const onClose = vi.fn();
    const { socket } = sessionWith({ onClose });
    socket.open();
    socket.drop(false);
    expect(onClose).toHaveBeenCalledWith('failed', expect.any(String));
  });

  it('names a closed-by-Vyzor session from its message', () => {
    const onClose = vi.fn();
    const { socket } = sessionWith({ onClose });
    socket.open();
    socket.text({ type: 'closed', reason: 'the session was idle for too long' });
    socket.drop(true);
    expect(onClose).toHaveBeenCalledWith('closed', expect.any(String));
  });

  it('skips a text message that is not JSON rather than throwing', () => {
    const onMessage = vi.fn();
    const { socket } = sessionWith({ onMessage });
    socket.open();
    expect(() => socket.onmessage?.({ data: 'not json' })).not.toThrow();
    expect(onMessage).not.toHaveBeenCalled();
  });
});

/**
 * The heartbeat.
 *
 * The engine gives up on a client it has not heard from, and the browser's own
 * pong is not a reliable sign of life — a backgrounded tab throttles its timers
 * and a sleeping phone stops answering entirely. So the console says "still
 * here" itself. These pin that it does, that it stops when the session ends, and
 * that returning to the foreground says so at once.
 */
describe('LiveSession heartbeat', () => {
  it('sends a ping on an interval while the socket is open', () => {
    vi.useFakeTimers();
    try {
      const { session, socket } = (() => {
        const s = new LiveSession('ws://vyzor.test/api/v1/interactive?url=x', {});
        s.connect();
        return { session: s, socket: FakeSocket.instances[0]! };
      })();
      socket.open();
      vi.advanceTimersByTime(20_000);
      expect(socket.sent).toEqual([JSON.stringify({ type: 'ping' })]);
      vi.advanceTimersByTime(20_000);
      expect(socket.sent).toHaveLength(2);
      session.close();
    } finally {
      vi.useRealTimers();
    }
  });

  it('stops heartbeating once the session is closed', () => {
    vi.useFakeTimers();
    try {
      const s = new LiveSession('ws://vyzor.test/api/v1/interactive?url=x', {});
      s.connect();
      const socket = FakeSocket.instances[0]!;
      socket.open();
      vi.advanceTimersByTime(20_000);
      const before = socket.sent.length;
      s.close();
      vi.advanceTimersByTime(60_000);
      expect(socket.sent.length).toBe(before);
    } finally {
      vi.useRealTimers();
    }
  });

  it('sends a ping when the tab returns to the foreground', () => {
    const listeners: Array<() => void> = [];
    const original = (globalThis as { document?: unknown }).document;
    (globalThis as { document?: unknown }).document = {
      visibilityState: 'visible',
      addEventListener: (_: string, fn: () => void) => listeners.push(fn),
      removeEventListener: () => {},
    };
    try {
      const s = new LiveSession('ws://vyzor.test/api/v1/interactive?url=x', {});
      s.connect();
      const socket = FakeSocket.instances[0]!;
      socket.open();
      expect(listeners).toHaveLength(1);
      listeners[0]!();
      expect(socket.sent).toContain(JSON.stringify({ type: 'ping' }));
      s.close();
    } finally {
      (globalThis as { document?: unknown }).document = original;
    }
  });
});
