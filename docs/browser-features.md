# Browser features

Vyzor can drive a real browser in three ways. Each is its own opt-in, because
each needs a browser and real memory.

| Feature | Flag | Route | What it does |
|---|---|---|---|
| Render | `--render` | `/api/v1/render` | draw a page once: a PNG, or its sanitised DOM |
| Live page | `--interactive` | `/api/v1/interactive` | hold a page open and drive it over a WebSocket |
| Live video | `--rtc` | `/api/v1/rtc` | stream a page as video over WebRTC |

A page is not its bytes. A site that builds itself with JavaScript arrives as a
document whose script must run before there is anything to show, and a site that
forbids being framed cannot be embedded at all. Hence a real browser.

## Render

```
GET /api/v1/render?url=https://example.com/&mode=image   # a picture
GET /api/v1/render?url=https://example.com/&mode=html    # the rendered DOM
```

`mode=html` is sanitised before it is served: an allowlist of tags and
attributes, no script or frame content, escaped attribute values, and a strict
`default-src 'none'` policy injected. Links are rewritten to route back through
the renderer so following one stays in the app. `X-Vyzorix-Sanitized: yes` says
whether that happened.

**Cost.** A render is a browser. Peak memory is roughly 500 MB, a typical page
takes 1–1.5 s, and concurrency is 1 by default. It needs at least 2 GB of RAM; on
a 512 MB instance it will be killed mid-render.

**The cache.** Rendered pages are cached under the work dir, keyed by URL and
mode. It is bounded — a byte budget, an entry count, LRU eviction, and a TTL —
because an unbounded cache is a slow disk leak. A cache hit is served before the
concurrency slot is taken, so a repeat view does not compete with a fresh one.
`--no-render-cache` turns it off.

## Live pages

Rendering draws a page once. That is the wrong shape for a page you want to
*use*: a picture has no search box you can type in and no link you can follow.
`--interactive` keeps a browser alive instead and streams it.

```
GET /api/v1/interactive?url=https://example.com/&w=1024&h=768   (WebSocket)
```

Frames go down, input comes up, over one socket. The frame protocol is in
[api.md](api.md).

### How it talks to the browser, and why

Over Chromium's `--remote-debugging-pipe`, **not** a debugging TCP port. A
debugging port is an unauthenticated control socket any process on the host could
drive; a pipe is private to Vyzor by construction. There is no WebSocket *client*
in Vyzor either — the CDP transport is two file descriptors, each message a
NUL-terminated JSON object.

Two details that matter more than they look:

- **Touch is not mouse.** A phone sends a finger, so the client forwards `touch`
  events, and `?touch=1` tells Vyzor to emulate a touch device. The client also
  sends the click a tap implies, because a field takes focus from that click and
  the on-screen keyboard follows the focus.
- **Focus emulation.** A headless browser has no window, so the page is treated
  as unfocused and a clicked field never takes the caret — typing then goes
  nowhere. `Emulation.setFocusEmulationEnabled` is set for every session.

### A vanished client

A tab closed behind a proxy can leave a connection that never reports EOF, and
the page may keep painting, so the idle timeout would never fire and the session
would hold its slot forever. Vyzor therefore pings an idle socket and drops a
client that has been silent too long.

## Live video over WebRTC

The live-page path streams **JPEG frames over a WebSocket**: capture, encode,
send, decode, draw. Every step is a cost, and the chain is gated on a CDP
acknowledgement round-trip, which caps the frame rate.

`--rtc` sends **raw video over WebRTC** instead:

```
chromium (software GL, on a real X display)
   | paints
Xvfb :99
   | ximagesrc
raw I420 --> vp8enc --> rtpvp8pay --> webrtcbin --SRTP--> browser
```

No JPEG, no base64, no per-frame acknowledgement, and a transport built for
media: SRTP over UDP with congestion control.

### Signalling

WebRTC cannot carry its own bootstrap. Here it is four plain HTTP requests
(see [api.md](api.md)). Input rides a WebRTC **data channel** labelled `input`,
inside the connection the video already uses, so the whole session is one
connection.

Gathering is awaited, not trickled: with no signalling socket there is nowhere
to send candidates mid-flight, so Vyzor waits for gathering to finish and puts
the candidates in the SDP. The cost is about a second at startup; the gain is a
handshake with no mid-flight messages.

### Requirements

- **Xvfb** — a virtual X server. Chromium is *not* headless here, because
  headless composites internally and never touches X, so there would be nothing
  for the capture to read.
- **GStreamer** with `ximagesrc`, `vp8enc`, `rtpvp8pay` and `webrtcbin`.
- **`libnice`** (`gstreamer1.0-nice`) — what gives `webrtcbin` ICE. Without it
  `webrtcbin` exists but cannot connect, and the failure appears as a null pad at
  runtime rather than a missing package, which is why Vyzor probes for the
  *elements* rather than the libraries.

When any is absent the capability reports unavailable and the console hides the
control, rather than accepting a session it cannot serve.

## Bounds, and what is not one

A render or a live session is a **resource-bounded subprocess, not a security
sandbox**. An unprivileged container cannot use Chromium's namespace sandbox, so
it runs with `--no-sandbox`. What is enforced:

- a hard wall-clock kill. Chromium's own `--virtual-time-budget` bounds *page*
  time and does nothing about a page that never settles, so Vyzor kills the
  child's whole process group at its own deadline;
- a per-render V8 heap cap (`ulimit -v` cannot be used — Chromium reserves a
  huge virtual address space and dies at once under it);
- a concurrency gate that refuses with `503` and `Retry-After` rather than
  queueing, because a queue behind a slow page grows without bound;
- an output size cap, a throwaway browser profile per render, and the process
  being a child subreaper so orphaned helpers are reaped;
- only public `http(s)` pages: `file://` would read this host's disk, and loopback
  and link-local addresses are refused so a render cannot reach Vyzor's own port
  or a cloud metadata endpoint.

**Run Vyzor where nothing sensitive lives.**
