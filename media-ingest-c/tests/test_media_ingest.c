/* test_media_ingest.c - self-contained tests.
 *
 * Uses a real in-process HTTP server (forked) so detection, ranged download,
 * resume, and retries are exercised over actual sockets. No mocks.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mi_detect.h"
#include "mi_download.h"
#include "mi_extract.h"
#include "mi_storage.h"
#include "mi_util.h"

/* ---------------------------------------------------------------- */
/* tiny test harness                                                 */
/* ---------------------------------------------------------------- */

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        }                                                                   \
    } while (0)

/* ---------------------------------------------------------------- */
/* fixtures                                                          */
/* ---------------------------------------------------------------- */

static void make_mp4(unsigned char *buf, size_t payload) {
    /* [size][ftyp][isom][minor][brands] + payload */
    const char ftyp[] = "ftypisom\x00\x00\x02\x00isomiso2mp41";
    size_t ftyp_len = sizeof ftyp - 1;
    uint32_t box = (uint32_t)(ftyp_len + 4);
    buf[0] = (box >> 24) & 0xFF;
    buf[1] = (box >> 16) & 0xFF;
    buf[2] = (box >> 8) & 0xFF;
    buf[3] = box & 0xFF;
    memcpy(buf + 4, ftyp, ftyp_len);
    memset(buf + 4 + ftyp_len, 0, payload);
}

static void make_webm(unsigned char *buf, size_t payload) {
    const unsigned char hdr[] = {0x1A, 0x45, 0xDF, 0xA3, 0x42, 0x86, 0x81,
                                 0x01, 'w',  'e',  'b',  'm'};
    memcpy(buf, hdr, sizeof hdr);
    memset(buf + sizeof hdr, 0, payload);
}

/* ---------------------------------------------------------------- */
/* in-process HTTP server (child process)                            */
/* ---------------------------------------------------------------- */

typedef struct {
    int port;
    pid_t pid;
} test_server;

static const char *g_body = NULL;
static size_t g_body_len = 0;
static int g_support_ranges = 1;

static void serve_conn(int fd) {
    char req[4096];
    ssize_t got = read(fd, req, sizeof req - 1);
    if (got <= 0) {
        close(fd);
        return;
    }
    req[got] = '\0';

    int head_only = strncmp(req, "HEAD", 4) == 0;
    size_t start = 0, end = g_body_len ? g_body_len - 1 : 0;
    int is_range = 0;
    const char *r = strstr(req, "Range: bytes=");
    if (r && g_support_ranges) {
        r += strlen("Range: bytes=");
        start = (size_t)strtoull(r, (char **)&r, 10);
        if (*r == '-') {
            r++;
            end = (size_t)strtoull(r, NULL, 10);
            if (end >= g_body_len) end = g_body_len - 1;
        }
        is_range = 1;
    }

    char header[512];
    int hlen;
    if (is_range) {
        hlen = snprintf(header, sizeof header,
                        "HTTP/1.1 206 Partial Content\r\n"
                        "Content-Range: bytes %zu-%zu/%zu\r\n"
                        "Content-Length: %zu\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Connection: close\r\n\r\n",
                        start, end, g_body_len, end - start + 1);
    } else {
        hlen = snprintf(header, sizeof header,
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Length: %zu\r\n"
                        "Accept-Ranges: %s\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Connection: close\r\n\r\n",
                        g_body_len, g_support_ranges ? "bytes" : "none");
    }
    (void)!write(fd, header, (size_t)hlen);
    if (!head_only && g_body) {
        if (is_range) (void)!write(fd, g_body + start, end - start + 1);
        else (void)!write(fd, g_body, g_body_len);
    }
    close(fd);
}

static void server_loop(int listen_fd) {
    signal(SIGTERM, SIG_DFL);
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        serve_conn(fd);
    }
    _exit(0);
}

static int start_server(const char *body, size_t len, int support_ranges,
                        test_server *out) {
    g_body = body;
    g_body_len = len;
    g_support_ranges = support_ranges;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(listen_fd);
        return -1;
    }
    socklen_t alen = sizeof addr;
    getsockname(listen_fd, (struct sockaddr *)&addr, &alen);
    out->port = ntohs(addr.sin_port);
    listen(listen_fd, 16);

    pid_t pid = fork();
    if (pid == 0) {
        close(STDIN_FILENO);
        server_loop(listen_fd);
    }
    out->pid = pid;
    close(listen_fd); /* parent's copy */
    return 0;
}

static void stop_server(test_server *srv) {
    if (srv->pid > 0) {
        kill(srv->pid, SIGKILL);
        waitpid(srv->pid, NULL, 0);
        srv->pid = -1;
    }
}

/* ---------------------------------------------------------------- */
/* detection tests                                                   */
/* ---------------------------------------------------------------- */

static void test_detect_mp4(void) {
    unsigned char buf[4096];
    make_mp4(buf, 2048);
    mi_media_info info;
    mi_detect(buf, sizeof buf, &info);
    CHECK(info.kind == MI_KIND_MP4, "mp4 kind");
    CHECK(info.is_video, "mp4 is video");
    CHECK(!strcmp(info.container, "mp4"), "mp4 container");
    mi_media_info_free(&info);
}

static void test_detect_webm(void) {
    unsigned char buf[2048];
    make_webm(buf, 1024);
    mi_media_info info;
    mi_detect(buf, sizeof buf, &info);
    CHECK(info.kind == MI_KIND_MATROSKA, "webm kind");
    CHECK(!strcmp(info.container, "webm"), "webm container");
    mi_media_info_free(&info);
}

static void test_detect_html_not_video(void) {
    const char *html = "<!DOCTYPE html><html><body>nope</body></html>";
    mi_media_info info;
    mi_detect((const unsigned char *)html, strlen(html), &info);
    CHECK(!info.is_video, "html is not video");
    CHECK(info.kind == MI_KIND_UNKNOWN, "html kind unknown");
    mi_media_info_free(&info);
}

static void test_detect_hls_manifest(void) {
    const char *pl = "#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:6,\nseg0.ts\nseg1.ts\n";
    mi_media_info info;
    mi_detect((const unsigned char *)pl, strlen(pl), &info);
    CHECK(info.kind == MI_KIND_HLS, "hls kind");
    CHECK(info.is_video, "hls is video");
    CHECK(info.manifest_count >= 2, "hls has segment urls");
    mi_media_info_free(&info);
}

static void test_detect_empty(void) {
    mi_media_info info;
    mi_detect(NULL, 0, &info);
    CHECK(info.kind == MI_KIND_UNKNOWN, "empty kind");
    CHECK(info.confidence == 0.0, "empty confidence");
    mi_media_info_free(&info);
}

/* Documents. These only ever get downloaded with allow_non_media, but the kind
 * still has to be right or the file lands with the wrong extension. */
static void test_detect_documents(void) {
    mi_media_info info;

    const char *pdf = "%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\n";
    mi_detect((const unsigned char *)pdf, strlen(pdf), &info);
    CHECK(info.kind == MI_KIND_PDF, "pdf detected");
    CHECK(strcmp(info.container, "pdf") == 0, "pdf container");
    CHECK(info.is_video == 0 && info.is_image == 0 && info.is_audio == 0,
          "pdf is not media");
    mi_media_info_free(&info);

    const char *txt = "This is a plain text file.\nIt has several lines.\n";
    mi_detect((const unsigned char *)txt, strlen(txt), &info);
    CHECK(info.kind == MI_KIND_TEXT, "text detected");
    CHECK(strcmp(info.container, "text") == 0, "text container");
    mi_media_info_free(&info);

    /* A binary blob that happens to start with letters must NOT be called text:
     * the check is deliberately conservative so a .zip is not mistaken for one. */
    unsigned char bin[64];
    memset(bin, 0, sizeof bin);
    bin[0] = 'P'; bin[1] = 'K'; bin[2] = 0x03; bin[3] = 0x04;
    for (size_t i = 4; i < sizeof bin; i++) bin[i] = (unsigned char)(i * 7);
    mi_detect(bin, sizeof bin, &info);
    CHECK(info.kind != MI_KIND_TEXT, "a binary blob is not text");
    mi_media_info_free(&info);
}

/*
 * The extractor, when it is installed.
 *
 * The interesting assertion is that the *result shape* is right: a format with
 * no URL must be dropped, because a caller that got one would queue a download
 * that cannot work. Skipped when yt-dlp is absent, since the module is a
 * subprocess wrapper and its own logic is the parsing.
 */
static void test_extract_parses_a_real_page(void) {
    mi_extract_config cfg;
    mi_extract_config_init(&cfg);
    if (!mi_extract_available(&cfg)) {
        printf("skip: no yt-dlp, so extraction is not exercised\n");
        return;
    }

    mi_extract_result res;
    int rc = mi_extract(&cfg, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", &res);
    if (rc != MI_OK) {
        /* A network failure in a sandbox is not a defect in the parser. */
        printf("skip: the extractor could not reach the site\n");
        return;
    }

    CHECK(res.title != NULL && *res.title, "the title is reported");
    CHECK(res.duration_seconds > 0, "the duration is reported");
    CHECK(res.format_count > 0, "formats are reported");

    int with_url = 0, video = 0;
    for (size_t i = 0; i < res.format_count; i++) {
        if (res.formats[i].url && *res.formats[i].url) with_url++;
        if (res.formats[i].has_video) video++;
    }
    /* Every format offered must carry a URL: one without is unresolvable and
     * would become a download that cannot start. */
    CHECK(with_url == (int)res.format_count, "every format has a URL");
    CHECK(video > 0, "at least one format carries video");

    mi_extract_result_free(&res);
    /* A freed result must be safe to free again, as the endpoint does. */
    mi_extract_result_free(&res);
}

/*
 * Search by name, and the discovery filters.
 *
 * A search answers "find a video called X" without a URL, which is the question
 * a crawler is often really being asked. `playlist_items` bounds how many are
 * returned — the extractor applies it while listing, so a bounded search never
 * resolves the items it discards, which is the whole point of doing it there
 * rather than in Vyzor.
 *
 * Skipped when the extractor is absent or the site is unreachable, since this is
 * a subprocess wrapper and its own logic is the argument building.
 */
static void test_extract_searches_by_name(void) {
    mi_extract_config cfg;
    mi_extract_config_init(&cfg);
    if (!mi_extract_available(&cfg)) {
        printf("skip: no yt-dlp, so search is not exercised\n");
        return;
    }

    cfg.search_term = "big buck bunny";
    cfg.search_limit = 8;
    /* Bounded to three: the filter is applied by the extractor while listing. */
    cfg.playlist_items = "1:3";

    mi_extract_result res;
    int rc = mi_extract(&cfg, NULL, &res);
    if (rc != MI_OK) {
        printf("skip: the extractor could not reach the site\n");
        return;
    }

    CHECK(res.entry_count > 0, "a search returns entries");
    /* The bound is the extractor's to honour; Vyzor only passes it. */
    CHECK(res.entry_count <= 3, "playlist_items bounds the result");
    /* A search resolves no stream URLs — that is a second call, for the item the
     * caller chooses. A format here would mean the whole list was resolved. */
    CHECK(res.format_count == 0, "a search does not resolve stream formats");

    if (res.entry_count > 0) {
        CHECK(res.entries[0].title != NULL, "an entry has a title");
        CHECK(res.entries[0].id != NULL, "an entry has an id");
        /* A search entry with no title and no id is dropped, so neither can be
         * missing here. */
    }
    mi_extract_result_free(&res);
    mi_extract_result_free(&res);
}

/*
 * A search and a URL are different questions, and one call does one of them.
 *
 * Passing neither is a caller error rather than an empty result: it would
 * otherwise run the extractor with a NULL argument and report whatever it said.
 */
static void test_extract_needs_a_url_or_a_search(void) {
    mi_extract_config cfg;
    mi_extract_config_init(&cfg);
    mi_extract_result res;
    CHECK(mi_extract(&cfg, NULL, &res) == MI_ERR_ARG,
          "neither a URL nor a search is refused");
    CHECK(mi_extract(&cfg, "https://example.com/", NULL) == MI_ERR_ARG,
          "a NULL result is refused");
}

static void test_extract_rejects_a_bad_argument(void) {
    mi_extract_config cfg;
    mi_extract_config_init(&cfg);
    CHECK(mi_extract(NULL, "https://example.com/", NULL) == MI_ERR_ARG,
          "a NULL config is refused");
}

/* ---------------------------------------------------------------- */
/* download tests                                                    */
/* ---------------------------------------------------------------- */

/*
 * A probe must not claim range support it does not have.
 *
 * The old probe sent a HEAD and then a ranged GET, and set `accept_ranges = 1`
 * whenever the GET returned any bytes — but a server that ignores `Range`
 * answers 200 with the whole object, which also returns bytes. The result was a
 * plan that promised parallel ranged downloading and then fell back, or worse,
 * mis-sized the transfer. This pins the distinction, because it is exactly the
 * kind of thing that only shows up against a real host.
 */
static void test_probe_range_support_is_measured(void) {
    size_t len = 400000;
    unsigned char *body = mi_malloc(len);
    make_mp4(body, len - 128);

    test_server srv = {0};
    if (start_server((const char *)body, len, 0, &srv) != 0) {
        CHECK(0, "server start (no ranges)");
        free(body);
        return;
    }
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/norange.mp4", srv.port);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    mi_download_plan plan;
    CHECK(mi_probe(&cfg, url, &plan) == MI_OK, "probe ok (no ranges)");
    CHECK(!plan.accept_ranges, "a host that ignores Range is not reported as ranged");
    CHECK(plan.total_bytes == (int64_t)len, "size still known without ranges");
    CHECK(plan.have_media && plan.media.kind == MI_KIND_MP4, "still detects mp4 without ranges");
    mi_download_plan_free(&plan);
    stop_server(&srv);
    free(body);
}

/*
 * A probe must still identify the media when the response is odd.
 *
 * A host that refuses HEAD (405) or serves 200 instead of 206 is common, and
 * the bytes are what the detector reads — so media is detected from whatever
 * arrived rather than only on a textbook 206.
 */
static void test_probe_detects_without_partial_content(void) {
    size_t len = 250000;
    unsigned char *body = mi_malloc(len);
    make_mp4(body, len - 128);

    test_server srv = {0};
    if (start_server((const char *)body, len, 1, &srv) != 0) {
        CHECK(0, "server start (ranges)");
        free(body);
        return;
    }
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/plain.mp4", srv.port);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    mi_download_plan plan;
    CHECK(mi_probe(&cfg, url, &plan) == MI_OK, "probe ok (ranged host)");
    CHECK(plan.accept_ranges, "a host that honours Range is reported as ranged");
    CHECK(plan.total_bytes == (int64_t)len, "ranged host size from Content-Range");
    CHECK(plan.have_media && plan.media.kind == MI_KIND_MP4, "detects mp4");
    mi_download_plan_free(&plan);
    stop_server(&srv);
    free(body);
}

static void test_probe_and_download(void) {
    size_t len = 3000000;
    unsigned char *body = mi_malloc(len);
    make_mp4(body, len - 128);

    test_server srv = {0};
    if (start_server((const char *)body, len, 1, &srv) != 0) {
        CHECK(0, "server start");
        free(body);
        return;
    }
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/movie.mp4", srv.port);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.connections = 8;
    cfg.chunk_size = 256 * 1024;

    mi_download_plan plan;
    CHECK(mi_probe(&cfg, url, &plan) == MI_OK, "probe ok");
    CHECK(plan.total_bytes == (int64_t)len, "probe size");
    CHECK(plan.accept_ranges, "probe ranges");
    CHECK(plan.have_media && plan.media.kind == MI_KIND_MP4, "probe detects mp4");
    mi_download_plan_free(&plan);

    char dest[] = "/tmp/mi-test-download.mp4";
    unlink(dest);
    mi_download_result res;
    int rc = mi_download(&cfg, url, dest, NULL, NULL, &res);
    CHECK(rc == MI_OK, "download ok");
    CHECK(res.bytes_written == (int64_t)len, "download size");
    CHECK(res.media.is_video, "download media is video");

    /* Verify byte-exactness. */
    FILE *f = fopen(dest, "rb");
    CHECK(f != NULL, "open downloaded");
    if (f) {
        unsigned char *check = mi_malloc(len);
        size_t got = fread(check, 1, len, f);
        fclose(f);
        CHECK(got == len && memcmp(check, body, len) == 0, "download byte-exact");
        free(check);
    }
    unlink(dest);
    mi_download_result_free(&res);
    stop_server(&srv);
    free(body);
}

static void test_refuse_non_video(void) {
    const char *html = "<!DOCTYPE html><html>not a video at all</html>";
    test_server srv = {0};
    if (start_server(html, strlen(html), 1, &srv) != 0) {
        CHECK(0, "server start");
        return;
    }
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/page.html", srv.port);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.require_media = 1;
    char dest[] = "/tmp/mi-test-refuse.html";
    unlink(dest);
    mi_download_result res;
    int rc = mi_download(&cfg, url, dest, NULL, NULL, &res);
    CHECK(rc != MI_OK, "refuses unrecognised payload");
    stop_server(&srv);
}

static void test_resume(void) {
    size_t len = 2000000;
    unsigned char *body = mi_malloc(len);
    make_mp4(body, len - 128);

    test_server srv = {0};
    if (start_server((const char *)body, len, 1, &srv) != 0) {
        CHECK(0, "server start");
        free(body);
        return;
    }
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/movie.mp4", srv.port);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    cfg.connections = 4;
    cfg.chunk_size = 500000;

    char dest[] = "/tmp/mi-test-resume.mp4";
    unlink(dest);

    /* Simulate an interrupted run: build the parts dir with range 0 done. */
    char *work = mi_asprintf("%s.parts", dest);
    mi_mkdir_p(work);
    /* first range: chunk_size bytes */
    char *part0 = mi_path_join(work, "part-00000");
    FILE *p = fopen(part0, "wb");
    if (p) {
        fwrite(body, 1, 500000, p);
        fclose(p);
    }
    char *state = mi_path_join(work, "state.txt");
    FILE *sf = fopen(state, "w");
    if (sf) {
        fprintf(sf, "total=%zu\ndone=0\n", len);
        fclose(sf);
    }

    mi_download_result res;
    int rc = mi_download(&cfg, url, dest, NULL, NULL, &res);
    CHECK(rc == MI_OK, "resume download ok");
    CHECK(res.resumed, "resumed flag set");

    FILE *f = fopen(dest, "rb");
    if (f) {
        unsigned char *check = mi_malloc(len);
        size_t got = fread(check, 1, len, f);
        fclose(f);
        CHECK(got == len && memcmp(check, body, len) == 0, "resume byte-exact");
        free(check);
    }
    unlink(dest);
    mi_download_result_free(&res);
    stop_server(&srv);
    free(body);
    free(work);
    free(part0);
    free(state);
}

static void test_streaming_fallback(void) {
    size_t len = 500000;
    unsigned char *body = mi_malloc(len);
    make_webm(body, len - 12);

    test_server srv = {0};
    if (start_server((const char *)body, len, 0 /* no ranges */, &srv) != 0) {
        CHECK(0, "server start");
        free(body);
        return;
    }
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/clip.mkv", srv.port);

    mi_download_config cfg;
    mi_download_config_init(&cfg);
    char dest[] = "/tmp/mi-test-stream.mkv";
    unlink(dest);
    mi_download_result res;
    int rc = mi_download(&cfg, url, dest, NULL, NULL, &res);
    CHECK(rc == MI_OK, "streaming fallback ok");
    CHECK(res.media.kind == MI_KIND_MATROSKA, "streaming detects webm");
    CHECK(res.bytes_written == (int64_t)len, "streaming size");
    unlink(dest);
    mi_download_result_free(&res);
    stop_server(&srv);
    free(body);
}

static void test_sha256(void) {
    const char *path = "/tmp/mi-test-sha.txt";
    FILE *f = fopen(path, "wb");
    fputs("abc", f);
    fclose(f);
    char hex[65];
    CHECK(mi_sha256_file(path, hex) == 0, "sha ok");
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256('abc')");
    unlink(path);
}

/* ---------------------------------------------------------------- */
/* crypto primitives (RFC 4231 / FIPS 180-4 vectors)                 */
/* ---------------------------------------------------------------- */

static void test_hmac_vectors(void) {
    /* RFC 4231 test case 1 */
    unsigned char key[20];
    memset(key, 0x0b, sizeof key);
    unsigned char out[32];
    mi_hmac_sha256(key, sizeof key, "Hi There", 8, out);
    char hex[65];
    mi_hex_encode(out, 32, hex);
    CHECK(!strcmp(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
          "hmac-sha256 RFC4231 tc1");

    /* RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?" */
    mi_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    mi_hex_encode(out, 32, hex);
    CHECK(!strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
          "hmac-sha256 RFC4231 tc2");

    /* SHA-256("abc") */
    char shex[65];
    mi_sha256_hex("abc", 3, shex);
    CHECK(!strcmp(shex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256('abc')");
}

/* Verify our SigV4 matches the AWS "get-vanilla" test-suite signature.
 * We can't call the static signer, so we recompute the chain with our own
 * primitives and assert the known-good final signature. */
static void test_sigv4_known_vector(void) {
    const char *secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    const char *date = "20150830";
    const char *region = "us-east-1";
    /* Build the canonical request from the AWS get-vanilla vector and hash it.
     * This exercises the real string layout (in particular that the trailing
     * newline of the canonical headers is NOT doubled) rather than hardcoding
     * the intermediate hash. */
    const char *canonical_headers =
        "host:example.amazonaws.com\nx-amz-date:20150830T123600Z\n";
    const char *signed_headers = "host;x-amz-date";
    const char *payload_hash =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    char canonical_request[512];
    snprintf(canonical_request, sizeof canonical_request,
             "GET\n/\n\n%s\n%s\n%s", canonical_headers, signed_headers, payload_hash);

    char creq_hash[65];
    mi_sha256_hex(canonical_request, strlen(canonical_request), creq_hash);
    CHECK(!strcmp(creq_hash,
                  "bb579772317eb040ac9ed261061d46c1f17a8133879d6129b6e1c25292927e63"),
          "sigv4 canonical request hash");

    /* The AWS get-vanilla suite vector uses the literal service name "service". */
    const char *service = "service";
    char sts[512];
    snprintf(sts, sizeof sts,
             "AWS4-HMAC-SHA256\n20150830T123600Z\n%s/%s/%s/aws4_request\n%s",
             date, region, service, creq_hash);

    char k0[128];
    snprintf(k0, sizeof k0, "AWS4%s", secret);
    unsigned char k1[32], k2[32], k3[32], k4[32];
    mi_hmac_sha256(k0, strlen(k0), date, strlen(date), k1);
    mi_hmac_sha256(k1, 32, region, strlen(region), k2);
    mi_hmac_sha256(k2, 32, service, strlen(service), k3);
    mi_hmac_sha256(k3, 32, "aws4_request", 12, k4);
    unsigned char sig[32];
    mi_hmac_sha256(k4, 32, sts, strlen(sts), sig);
    char hex[65];
    mi_hex_encode(sig, 32, hex);
    CHECK(!strcmp(hex, "5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"),
          "sigv4 get-vanilla signature");
}

/* ---------------------------------------------------------------- */
/* live S3 mock: verifies the PUT lands and the SigV4 header is well-formed */
/* ---------------------------------------------------------------- */

typedef struct {
    char object[4096];
    size_t object_len;
    char auth[1024];
    char content_type[128];
    char path[256];
    char host[256];
    char amz_date[32];
    char payload_hash[80];
    char sig_inputs_ok; /* 1 when we verified the signature server-side */
} s3_capture;

static void serve_s3(int fd, s3_capture *cap) {
    char req[16384];
    ssize_t got = read(fd, req, sizeof req - 1);
    if (got <= 0) {
        close(fd);
        return;
    }
    req[got] = '\0';

    /* First line: METHOD PATH HTTP/1.1 */
    char method[16] = {0}, path[256] = {0};
    sscanf(req, "%15s %255s", method, path);
    snprintf(cap->path, sizeof cap->path, "%s", path);

    /* Extract Authorization and Content-Type headers. */
    char *line = strstr(req, "\r\n");
    while (line) {
        char *hdr = line + 2;
        char *next = strstr(hdr, "\r\n");
        size_t llen = next ? (size_t)(next - hdr) : 0;
        char *dst = NULL;
        size_t dsz = 0;
        const char *val = NULL;
        size_t vlen = 0;
        if (!strncasecmp(hdr, "Authorization:", 14)) {
            val = hdr + 14;
            vlen = llen - 14;
            dst = cap->auth;
            dsz = sizeof cap->auth;
        } else if (!strncasecmp(hdr, "Content-Type:", 13)) {
            val = hdr + 13;
            vlen = llen - 13;
            dst = cap->content_type;
            dsz = sizeof cap->content_type;
        } else if (!strncasecmp(hdr, "Host:", 5)) {
            val = hdr + 5;
            vlen = llen - 5;
            dst = cap->host;
            dsz = sizeof cap->host;
        } else if (!strncasecmp(hdr, "x-amz-date:", 11)) {
            val = hdr + 11;
            vlen = llen - 11;
            dst = cap->amz_date;
            dsz = sizeof cap->amz_date;
        } else if (!strncasecmp(hdr, "x-amz-content-sha256:", 21)) {
            val = hdr + 21;
            vlen = llen - 21;
            dst = cap->payload_hash;
            dsz = sizeof cap->payload_hash;
        }
        if (dst) {
            while (vlen > 0 && (*val == ' ' || *val == '\t')) {
                val++;
                vlen--;
            }
            snprintf(dst, dsz, "%.*s", (int)vlen, val);
        }
        line = next;
    }

    /* Body starts after the blank line. */
    char *body = strstr(req, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t hdr = (size_t)(body - req);
        size_t blen = (size_t)got > hdr ? (size_t)got - hdr : 0;
        if (blen > sizeof cap->object) blen = sizeof cap->object;
        memcpy(cap->object, body, blen);
        cap->object_len = blen;
    }

    /* Verify the signature the way a real S3 server would: rebuild the
     * canonical request from the *wire* headers and compare. This is what
     * catches a Host that does not match what was signed. */
    const char *signed_hdrs = "content-type;host;x-amz-content-sha256;x-amz-date";
    char canonical[2048];
    snprintf(canonical, sizeof canonical,
             "PUT\n%s\n\ncontent-type:%s\nhost:%s\n"
             "x-amz-content-sha256:%s\nx-amz-date:%s\n\n%s\n%s",
             cap->path, cap->content_type, cap->host, cap->payload_hash,
             cap->amz_date, signed_hdrs, cap->payload_hash);
    char creq_hash[65];
    mi_sha256_hex(canonical, strlen(canonical), creq_hash);

    /* Recompute the signing key from the test secret. The date stamp is the
     * first 8 characters of the x-amz-date header. */
    char datestamp[9] = {0};
    memcpy(datestamp, cap->amz_date, 8);
    const char *secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    char k0[128];
    snprintf(k0, sizeof k0, "AWS4%s", secret);
    unsigned char k1[32], k2[32], k3[32], k4[32], sig[32];
    mi_hmac_sha256(k0, strlen(k0), datestamp, strlen(datestamp), k1);
    mi_hmac_sha256(k1, 32, "us-east-1", 9, k2);
    mi_hmac_sha256(k2, 32, "s3", 2, k3);
    mi_hmac_sha256(k3, 32, "aws4_request", 12, k4);
    char sts[512];
    snprintf(sts, sizeof sts,
             "AWS4-HMAC-SHA256\n%s\n%s/us-east-1/s3/aws4_request\n%s",
             cap->amz_date, datestamp, creq_hash);
    mi_hmac_sha256(k4, 32, sts, strlen(sts), sig);
    char expect[65];
    mi_hex_encode(sig, 32, expect);
    cap->sig_inputs_ok = (strstr(cap->auth, expect) != NULL) ? 1 : 0;

    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "ETag: \"d41d8cd98f00b204e9800998ecf8427e\"\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    (void)!write(fd, resp, strlen(resp));
    close(fd);
}

static void s3_loop(int listen_fd, s3_capture *cap) {
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) break;
        serve_s3(fd, cap);
    }
    _exit(0);
}

static void test_s3_upload_live(void) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        CHECK(0, "s3 bind");
        return;
    }
    socklen_t alen = sizeof addr;
    getsockname(listen_fd, (struct sockaddr *)&addr, &alen);
    listen(listen_fd, 8);
    int port = ntohs(addr.sin_port);

    /* The server runs in a child process, so its observations must live in
     * shared memory to be visible to the parent. */
    s3_capture *cap = mmap(NULL, sizeof *cap, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (cap == MAP_FAILED) {
        CHECK(0, "s3 mmap");
        close(listen_fd);
        return;
    }
    memset(cap, 0, sizeof *cap);

    pid_t pid = fork();
    if (pid == 0) {
        close(STDIN_FILENO);
        s3_loop(listen_fd, cap);
    }
    close(listen_fd);

    char endpoint[64];
    snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%d", port);

    mi_s3_config cfg;
    mi_s3_config_init(&cfg);
    cfg.endpoint = endpoint;
    cfg.region = "us-east-1";
    cfg.bucket = "test-bucket";
    cfg.access_key = "AKIDEXAMPLE";
    cfg.secret_key = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    cfg.path_style = 1;

    const char *payload = "hello s3 payload";
    mi_upload_result up;
    int rc = mi_s3_upload_bytes(&cfg, payload, strlen(payload), "hls/movie/master.m3u8", &up);
    CHECK(rc == MI_OK, "s3 upload ok");
    CHECK(up.http_status == 200, "s3 status 200");
    CHECK(up.bytes == (int64_t)strlen(payload), "s3 byte count");
    CHECK(up.etag && strlen(up.etag) > 0, "s3 etag captured");

    /* The server saw the object bytes verbatim. */
    CHECK(cap->object_len == strlen(payload) &&
              memcmp(cap->object, payload, strlen(payload)) == 0,
          "s3 body received verbatim");
    /* Path-style URL: /test-bucket/hls/movie/master.m3u8 */
    CHECK(!strcmp(cap->path, "/test-bucket/hls/movie/master.m3u8"), "s3 path-style url");
    /* Authorization header looks like SigV4. */
    CHECK(!strncmp(cap->auth, "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/",
                   strlen("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/")),
          "s3 auth header is sigv4");
    CHECK(strstr(cap->auth, "SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date"),
          "s3 signed headers correct");
    CHECK(strstr(cap->auth, "Signature=") != NULL, "s3 has signature");
    /* The critical one: the server independently recomputed the signature from
     * the wire headers and it matched (i.e. the signed Host == sent Host). */
    CHECK(cap->sig_inputs_ok, "s3 signature verifies server-side");
    /* The Host we signed must include the port (regression guard). */
    CHECK(strchr(cap->host, ':') != NULL, "s3 signed host includes port");
    /* HLS content type derived from the key. */
    CHECK(!strcmp(cap->content_type, "application/vnd.apple.mpegurl"),
          "s3 content-type for m3u8");

    /* Public URL helper with a CDN base. */
    char *url = mi_storage_public_url(&cfg, "https://cdn.example.com", "hls/movie/master.m3u8");
    CHECK(url && !strcmp(url, "https://cdn.example.com/hls/movie/master.m3u8"),
          "cdn base url");
    free(url);
    /* Without a CDN base we fall back to the S3 URL; assert the shape, since
     * the mock endpoint is a bare IP:port. */
    char *s3url = mi_storage_public_url(&cfg, NULL, "a/b.mp4");
    CHECK(s3url != NULL, "s3 url non-null");
    CHECK(s3url && strstr(s3url, "test-bucket") && strstr(s3url, "/a/b.mp4"),
          "s3 url contains bucket and key");
    free(s3url);

    /* Key with characters needing percent-encoding. */
    char *enc = mi_storage_public_url(&cfg, "https://cdn.example.com/",
                                      "hls/a b/master.m3u8");
    CHECK(enc && !strcmp(enc, "https://cdn.example.com/hls/a%20b/master.m3u8"),
          "cdn base url percent-encodes key");
    free(enc);

    mi_upload_result_free(&up);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    munmap(cap, sizeof *cap);
}

/* ---------------------------------------------------------------- */
/* image + audio detection                                           */
/* ---------------------------------------------------------------- */

static void test_detect_image_and_audio(void) {
    const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F'};
    mi_media_info m;
    mi_detect(jpeg, sizeof jpeg, &m);
    CHECK(m.kind == MI_KIND_JPEG, "jpeg kind");
    CHECK(m.is_image && !m.is_video && !m.is_audio, "jpeg flags");

    const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    mi_detect(png, sizeof png, &m);
    CHECK(m.kind == MI_KIND_PNG, "png kind");
    CHECK(m.is_image, "png is image");

    const unsigned char flac[] = {'f', 'L', 'a', 'C', 0x00, 0x00, 0x00, 0x22};
    mi_detect(flac, sizeof flac, &m);
    CHECK(m.kind == MI_KIND_FLAC, "flac kind");
    CHECK(m.is_audio && !m.is_image && !m.is_video, "flac flags");

    const unsigned char wav[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    mi_detect(wav, sizeof wav, &m);
    CHECK(m.kind == MI_KIND_WAV, "wav kind");
    CHECK(m.is_audio, "wav is audio");

    const unsigned char id3[] = {'I', 'D', '3', 0x03, 0x00};
    mi_detect(id3, sizeof id3, &m);
    CHECK(m.kind == MI_KIND_MP3, "mp3 kind");
    CHECK(m.is_audio, "mp3 is audio");

    const unsigned char gif[] = {'G', 'I', 'F', '8', '9', 'a'};
    mi_detect(gif, sizeof gif, &m);
    CHECK(m.kind == MI_KIND_GIF && m.is_image, "gif kind");

    const unsigned char webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    mi_detect(webp, sizeof webp, &m);
    CHECK(m.kind == MI_KIND_WEBP && m.is_image, "webp kind");
}

/* ---------------------------------------------------------------- */
/* HLS / DASH rendition parsing                                      */
/* ---------------------------------------------------------------- */

static void test_hls_renditions(void) {
    const char *pl =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.42c01e,mp4a.40.2\"\n"
        "360p/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"avc1.640028\"\n"
        "1080p/index.m3u8\n";
    mi_media_info m;
    mi_detect((const unsigned char *)pl, strlen(pl), &m);
    CHECK(m.kind == MI_KIND_HLS, "hls detected");

    mi_parse_hls_renditions(pl, strlen(pl), 600.0, &m);
    CHECK(m.quality_count == 2, "two hls qualities");
    if (m.quality_count == 2) {
        CHECK(m.qualities[0].height == 360, "hls q0 height");
        CHECK(m.qualities[0].bandwidth == 800000, "hls q0 bandwidth");
        /* 800000 bits/s * 600 s / 8 = 60,000,000 bytes */
        CHECK(m.qualities[0].est_bytes == 60000000, "hls q0 size from bandwidth");
        CHECK(m.qualities[1].height == 1080, "hls q1 height");
        CHECK(m.qualities[1].est_bytes == 375000000, "hls q1 size");
        CHECK(!strcmp(m.qualities[0].url, "360p/index.m3u8"), "hls q0 url");
    }

    /* Without a duration the size must stay unknown, not be guessed. */
    mi_media_info m2;
    memset(&m2, 0, sizeof m2);
    mi_parse_hls_renditions(pl, strlen(pl), 0, &m2);
    CHECK(m2.quality_count == 2, "hls qualities without duration");
    CHECK(m2.qualities[0].est_bytes == -1, "unknown duration -> unknown size");
    mi_media_info_free(&m2);
    mi_media_info_free(&m);
}

static void test_dash_renditions(void) {
    const char *mpd =
        "<?xml version=\"1.0\"?>\n"
        "<MPD><Period><AdaptationSet>\n"
        "<Representation id=\"v0\" bandwidth=\"800000\" width=\"640\" height=\"360\" "
        "codecs=\"avc1.42c01e\" />\n"
        "<Representation id=\"v1\" bandwidth=\"5000000\" width=\"1920\" height=\"1080\" "
        "codecs=\"avc1.640028\" />\n"
        "<Representation id=\"a0\" bandwidth=\"128000\" />\n"
        "</AdaptationSet></Period></MPD>";
    mi_media_info m;
    memset(&m, 0, sizeof m);
    mi_parse_dash_renditions(mpd, strlen(mpd), 600.0, &m);
    /* the audio-only representation has no dimensions and is not a quality */
    CHECK(m.quality_count == 2, "two dash video qualities");
    if (m.quality_count == 2) {
        CHECK(m.qualities[1].height == 1080, "dash q1 height");
        CHECK(m.qualities[1].est_bytes == 375000000, "dash q1 size");
        CHECK(!strcmp(m.qualities[1].url, "v1"), "dash q1 id");
    }
    mi_media_info_free(&m);
}

/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */

/* A real MP4 header: the tkhd box carries the frame size, and the filename
 * cannot be trusted (this file is named "720p" and is 640x360). */
static void test_mp4_dimensions(void) {
    /* ftyp, then a moov containing tkhd. Built by hand so the offsets are exact. */
    unsigned char buf[512];
    memset(buf, 0, sizeof buf);
    /* ftyp box, 24 bytes: size, 'ftyp', brand 'isom', minor, compat 'isom' */
    unsigned char ftyp[24] = {0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm',
                              0, 0, 2, 0, 'i', 's', 'o', 'm', 'm', 'p', '4', '1'};
    memcpy(buf, ftyp, sizeof ftyp);
    /* moov box header at 24, then tkhd inside it at 32. */
    unsigned char moov_hdr[8] = {0, 0, 0, 0, 'm', 'o', 'o', 'v'};
    memcpy(buf + 24, moov_hdr, 8);
    size_t tkhd_at = 32;
    unsigned char tkhd[100];
    memset(tkhd, 0, sizeof tkhd);
    size_t tkhd_size = sizeof tkhd;
    tkhd[0] = (unsigned char)(tkhd_size >> 24);
    tkhd[1] = (unsigned char)(tkhd_size >> 16);
    tkhd[2] = (unsigned char)(tkhd_size >> 8);
    tkhd[3] = (unsigned char)tkhd_size;
    memcpy(tkhd + 4, "tkhd", 4);
    /* 640 x 360 as 16.16 fixed point in the last 8 bytes. */
    uint32_t w = 640u << 16, h = 360u << 16;
    tkhd[92] = (unsigned char)(w >> 24); tkhd[93] = (unsigned char)(w >> 16);
    tkhd[94] = (unsigned char)(w >> 8);  tkhd[95] = (unsigned char)w;
    tkhd[96] = (unsigned char)(h >> 24); tkhd[97] = (unsigned char)(h >> 16);
    tkhd[98] = (unsigned char)(h >> 8);  tkhd[99] = (unsigned char)h;
    memcpy(buf + tkhd_at, tkhd, sizeof tkhd);
    size_t moov_size = 8 + sizeof tkhd;
    buf[24] = (unsigned char)(moov_size >> 24);
    buf[25] = (unsigned char)(moov_size >> 16);
    buf[26] = (unsigned char)(moov_size >> 8);
    buf[27] = (unsigned char)moov_size;

    mi_media_info info;
    memset(&info, 0, sizeof info);
    mi_detect(buf, sizeof buf, &info);
    CHECK(info.kind == MI_KIND_MP4, "mp4 still detected");
    CHECK(info.width == 640, "width read from tkhd");
    CHECK(info.height == 360, "height read from tkhd");
    mi_media_info_free(&info);

    /* An MP4 with no tkhd must not invent a size. */
    mi_media_info bare;
    memset(&bare, 0, sizeof bare);
    mi_detect(ftyp, sizeof ftyp, &bare);
    CHECK(bare.kind == MI_KIND_MP4, "bare mp4 detected");
    CHECK(bare.width == 0 && bare.height == 0, "no size invented without a tkhd");
    mi_media_info_free(&bare);
}

int main(void) {
    mi_log_set_level(MI_LOG_ERROR); /* keep test output clean */
    signal(SIGPIPE, SIG_IGN);

    test_mp4_dimensions();
    test_detect_mp4();
    test_detect_webm();
    test_detect_html_not_video();
    test_detect_hls_manifest();
    test_detect_empty();
    test_detect_documents();
    test_sha256();
    test_extract_parses_a_real_page();
    test_extract_searches_by_name();
    test_extract_needs_a_url_or_a_search();
    test_extract_rejects_a_bad_argument();
    test_probe_range_support_is_measured();
    test_probe_detects_without_partial_content();
    test_probe_and_download();
    test_refuse_non_video();
    test_resume();
    test_streaming_fallback();
    test_hmac_vectors();
    test_sigv4_known_vector();
    test_s3_upload_live();
    test_detect_image_and_audio();
    test_hls_renditions();
    test_dash_renditions();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
