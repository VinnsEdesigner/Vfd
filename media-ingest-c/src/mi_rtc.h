/* mi_rtc.h - a live page streamed as raw video over WebRTC.
 *
 * WHY THIS REPLACES THE JPEG PATH
 *
 * The previous live view pushed JPEG frames over a WebSocket: capture the page
 * with CDP screencast, encode JPEG, send, decode in the browser, draw into an
 * <img>. Every step is a cost, and the whole chain is gated on a CDP
 * acknowledgement round-trip, which caps the frame rate.
 *
 * This sends raw video instead, over WebRTC:
 *
 *   chromium (software GL, on a real X display)
 *      | paints
 *   Xvfb :99
 *      | ximagesrc
 *   raw I420 --> vp8enc --> rtpvp8pay --> webrtcbin --SRTP--> browser
 *
 * No JPEG, no base64, no per-frame acknowledgement, and a transport built for
 * media: SRTP over UDP with congestion control, rather than a TCP stream that
 * head-of-line blocks.
 *
 * NO WEBSOCKET
 *
 * WebRTC has no signalling of its own — it deliberately leaves that to the
 * application, because the two peers have to agree on *something* before a peer
 * connection exists. That something cannot be WebRTC.
 *
 * Here it is plain HTTP, one request per step, so there is no long-lived socket
 * anywhere:
 *
 *   POST   /api/v1/rtc              -> { token, sdp, viewport }   (the offer)
 *   POST   /api/v1/rtc/{token}/answer { sdp }                     (the answer)
 *   GET    /api/v1/rtc/{token}      -> { state, connected, ice }   (poll)
 *   DELETE /api/v1/rtc/{token}                                    (close)
 *
 * Input does not need a socket either: it rides a WebRTC data channel, which is
 * a channel inside the same peer connection the video already uses. So the whole
 * session — media and control — is one WebRTC connection, and HTTP is used only
 * for the handful of messages that establish it.
 *
 * That also removes the need to trickle ICE. Trickle exists to avoid waiting for
 * gathering, but with no socket to trickle *on*, waiting is simpler: the offer is
 * returned only once gathering has finished, so its candidates are in the SDP and
 * the browser needs no separate candidate messages at all.
 *
 * THE TWO THINGS THAT MAKE IT POSSIBLE, BOTH MEASURED
 *
 *   - Chromium must run on a real display, NOT `--headless`. Headless composites
 *     internally and never touches X, so there is nothing to capture. It needs
 *     `--use-gl=swiftshader` because the virtual display has no GPU.
 *   - `ximagesrc` advertises `video/x-raw` with no fixed format and negotiates;
 *     demanding a format at the source fails. Convert after it.
 *
 * WHAT THIS IS NOT
 *
 * Not a security sandbox. Like the JPEG path it runs Chromium with --no-sandbox,
 * because an unprivileged container cannot use Chromium's namespace sandbox. The
 * bounds that are real are the same ones: a concurrency gate, an idle timeout, a
 * hard wall-clock limit, and a throwaway profile. Run it where nothing sensitive
 * lives.
 */
#ifndef MI_RTC_H
#define MI_RTC_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MI_RTC_OK = 0,
    MI_RTC_BUSY,        /* the gate is full */
    MI_RTC_NO_PLUGIN,   /* GStreamer, webrtcbin, ximagesrc or Xvfb is missing */
    MI_RTC_BAD_URL,     /* not a renderable http(s) URL */
    MI_RTC_FAILED       /* the pipeline could not be built or started */
} mi_rtc_status;

typedef struct {
    const char *chromium_path;   /* NULL searches the usual names */
    const char *xvfb_path;       /* NULL searches PATH */
    const char *display;         /* e.g. ":99"; NULL picks one */
    int max_concurrent;          /* sessions at once; each is a browser + X server */
    int v8_heap_mb;
    int width;
    int height;
    /* Target frame rate for the capture. ximagesrc can produce far more than a
     * viewer needs, and every extra frame costs an encode. */
    int fps;
    /* Encoder bitrate in kbit/s. VP8 at 800x600 needs a few hundred. */
    int bitrate_kbps;
    const char *user_agent;
    const char *profile_root;
    int idle_timeout_seconds;
    int startup_timeout_seconds;
    /* STUN server for ICE, e.g. "stun://stun.l.google.com:19302". NULL uses
     * host candidates only, which works on a LAN and not across one. */
    const char *stun_server;
    /*
     * How long to wait for ICE gathering to finish before returning the offer.
     *
     * Gathering is not instant: the browser has to try each interface and, with
     * a STUN server, wait for a reply. Because there is no socket to trickle on,
     * the offer is only handed over once this has settled, so the wait is the
     * price of a single-request handshake. A host with no STUN gathers in
     * milliseconds; one behind a NAT may use the whole budget.
     */
    int gather_timeout_ms;
} mi_rtc_config;

void mi_rtc_config_init(mi_rtc_config *cfg);

typedef struct mi_rtc_pool mi_rtc_pool;

mi_rtc_pool *mi_rtc_pool_new(const mi_rtc_config *cfg);
void mi_rtc_pool_free(mi_rtc_pool *pool);

/* Whether a browser, Xvfb, and the GStreamer plugins are all present. */
int mi_rtc_pool_available(const mi_rtc_pool *pool);
int mi_rtc_pool_limit(const mi_rtc_pool *pool);
int mi_rtc_pool_active(const mi_rtc_pool *pool);

/* ------------------------------------------------------------------ */
/* A session                                                           */
/* ------------------------------------------------------------------ */

typedef struct mi_rtc mi_rtc;

/*
 * Start a session on `url` and return immediately with a handle.
 *
 * This spawns the X server, the browser and the pipeline, which takes a moment;
 * the offer is not ready yet. Call `mi_rtc_wait_offer` to block until it is. The
 * split exists because the HTTP layer wants to answer "starting" separately from
 * "here is the offer", rather than holding a request open for the whole startup.
 *
 * The session takes a gate slot for its whole life.
 */
mi_rtc_status mi_rtc_open(mi_rtc_pool *pool, const char *url, mi_rtc **out,
                          char *err, size_t errcap);

/*
 * Pump until the offer is ready or `timeout_ms` passes. Returns 0 when the offer
 * is ready, -1 otherwise (ended, or the deadline passed).
 *
 * The offer is only complete once ICE gathering has finished, so this waits for
 * both — see `gather_timeout_ms`.
 */
int mi_rtc_wait_offer(mi_rtc *rtc, int timeout_ms);

/* The local description, complete with its gathered candidates. Allocated;
 * free it. NULL until `mi_rtc_wait_offer` has returned 0. */
char *mi_rtc_offer(mi_rtc *rtc);

/* Whether ICE gathering has finished, so the offer is final. */
int mi_rtc_gathering_done(mi_rtc *rtc);

/*
 * Apply the browser's answer.
 *
 * Returns 0, or -1 when the SDP could not be parsed. With gathering complete on
 * both sides this is the last signalling step: after it the connection negotiates
 * DTLS and media starts flowing.
 */
int mi_rtc_set_answer(mi_rtc *rtc, const char *sdp);

/*
 * Pump the pipeline for up to `ms`.
 *
 * Returns 0 while the session is alive, -1 once it has ended. Must be called
 * regularly: this is what runs the GStreamer main loop, so a session that is not
 * pumped does not negotiate, does not send, and never notices that its browser
 * has gone.
 */
int mi_rtc_pump(mi_rtc *rtc, int ms);

/* The connection's state, for reporting: "new", "connecting", "connected",
 * "disconnected", "failed", "closed". Never NULL. */
const char *mi_rtc_state(mi_rtc *rtc);

/* Whether media is actually flowing (the connection reached `connected`). */
int mi_rtc_connected(mi_rtc *rtc);

/* Why the session ended, for a farewell. Never NULL. */
const char *mi_rtc_close_reason(mi_rtc *rtc);

/* The browser's viewport, which is the X display's size. */
void mi_rtc_viewport(mi_rtc *rtc, int *width, int *height);

/* Whether this URL may be streamed. The same rules as rendering and the JPEG
 * live path: public http(s) only, so a stream cannot reach this host's disk or
 * its own port. */
int mi_rtc_url_allowed(const char *url);

/* Close the session and release its slot. Idempotent; safe on NULL. */
void mi_rtc_close(mi_rtc *rtc);

#endif /* MI_RTC_H */
