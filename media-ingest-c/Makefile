# media-ingest-c
CC      ?= cc
# GStreamer is what carries raw video over WebRTC. Its WebRTC API lives in the
# "bad" plugin set and prints an "unstable API" warning unless asked not to;
# GST_USE_UNSTABLE_API silences that. webrtcbin is the only viable C option:
# libdatachannel is unpackaged in Debian, and libwebrtc is C++ only.
GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 2>/dev/null)
GST_LIBS   := $(shell pkg-config --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 2>/dev/null)

CFLAGS  ?= -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
           -Wmissing-prototypes -std=c11 -DGST_USE_UNSTABLE_API -Isrc $(GST_CFLAGS)
LDFLAGS ?=
LDLIBS  := -lcurl -lpthread -lsqlite3 -lz $(GST_LIBS)

SRC_DIR := src
BUILD   := build
BIN     := $(BUILD)/vyzor
LIB_SRC := $(SRC_DIR)/mi_util.c $(SRC_DIR)/mi_detect.c \
           $(SRC_DIR)/mi_download.c $(SRC_DIR)/mi_hls.c \
           $(SRC_DIR)/mi_storage.c $(SRC_DIR)/mi_json.c \
           $(SRC_DIR)/mi_browse.c $(SRC_DIR)/mi_render.c $(SRC_DIR)/mi_cache.c \
           $(SRC_DIR)/mi_sanitize.c $(SRC_DIR)/mi_identify.c $(SRC_DIR)/mi_outbox.c $(SRC_DIR)/mi_rtc.c $(SRC_DIR)/mi_ws.c $(SRC_DIR)/mi_interactive.c $(SRC_DIR)/mi_mediawatch.c \
           $(SRC_DIR)/mi_crawl.c $(SRC_DIR)/mi_extract.c \
           $(SRC_DIR)/mi_identity.c $(SRC_DIR)/mi_captcha.c $(SRC_DIR)/mi_turso.c \
           $(SRC_DIR)/mi_impersonate.c $(SRC_DIR)/mi_antibot.c \
           $(SRC_DIR)/mi_browser_headers.c $(SRC_DIR)/mi_politeness.c \
           $(SRC_DIR)/mi_html.c $(SRC_DIR)/mi_html5.c $(SRC_DIR)/mi_css.c $(SRC_DIR)/mi_xpath.c \
           $(SRC_DIR)/mi_charset.c $(SRC_DIR)/mi_structured.c $(SRC_DIR)/mi_sitemap.c \
           $(SRC_DIR)/mi_fingerprint.c $(SRC_DIR)/mi_cookiejar.c $(SRC_DIR)/mi_form.c \
           $(SRC_DIR)/mi_session.c $(SRC_DIR)/mi_linkcheck.c $(SRC_DIR)/mi_search.c \
           $(SRC_DIR)/mi_lang.c $(SRC_DIR)/mi_captcha_inject.c \
           $(SRC_DIR)/mi_extract_rows.c \
           $(SRC_DIR)/mi_torrent.c \
           $(SRC_DIR)/mi_frontier.c \
           $(SRC_DIR)/mi_server.c
CLI_SRC := $(SRC_DIR)/mi_cli.c
TEST_SRC := tests/test_media_ingest.c
TEST_SERVER_SRC := tests/test_server.c
TEST_RENDER_SRC := tests/test_render.c
TEST_CACHE_SRC := tests/test_cache.c
TEST_SANITIZE_SRC := tests/test_sanitize.c
TEST_WS_SRC := tests/test_ws.c
TEST_INTERACTIVE_SRC := tests/test_interactive.c
TEST_IDENTIFY_SRC := tests/test_identify.c
TEST_OUTBOX_SRC := tests/test_outbox.c
TEST_RTC_SRC := tests/test_rtc.c
TEST_CRAWL_SRC := tests/test_crawl.c
TEST_EXTRACT_SRC := tests/test_extract.c
TEST_IDENTITY_SRC := tests/test_identity.c
TEST_CAPTCHA_SRC := tests/test_captcha.c
TEST_FRONTIER_SRC := tests/test_frontier.c
TEST_IMPERSONATE_SRC := tests/test_impersonate.c
TEST_ANTIBOT_SRC := tests/test_antibot.c
TEST_BROWSER_HEADERS_SRC := tests/test_browser_headers.c
TEST_POLITENESS_SRC := tests/test_politeness.c
TEST_CONDGET_SRC := tests/test_condget.c
TEST_HTML_SRC := tests/test_html.c
TEST_CHARSET_SRC := tests/test_charset.c
TEST_STRUCTURED_SRC := tests/test_structured.c
TEST_SITEMAP_SRC := tests/test_sitemap.c
TEST_FINGERPRINT_SRC := tests/test_fingerprint.c
TEST_COOKIEJAR_SRC := tests/test_cookiejar.c
TEST_FORM_SRC := tests/test_form.c
TEST_SESSION_SRC := tests/test_session.c
TEST_LINKCHECK_SRC := tests/test_linkcheck.c
TEST_SEARCH_SRC := tests/test_search.c
TEST_LANG_SRC := tests/test_lang.c
TEST_CAPTCHA_INJECT_SRC := tests/test_captcha_inject.c
TEST_HTML5_SRC := tests/test_html5.c
TEST_CSS_SRC := tests/test_css.c
TEST_XPATH_SRC := tests/test_xpath.c
TEST_EXTRACT_ROWS_SRC := tests/test_extract_rows.c
TEST_TORRENT_SRC := tests/test_torrent.c

.PHONY: all clean test

all: $(BIN)

$(BUILD):
	@mkdir -p $(BUILD)

$(BIN): $(LIB_SRC) $(CLI_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(CLI_SRC) $(LDLIBS)

$(BUILD)/test_media_ingest: $(LIB_SRC) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_SRC) $(LDLIBS)

$(BUILD)/test_server: $(LIB_SRC) $(TEST_SERVER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_SERVER_SRC) $(LDLIBS)

$(BUILD)/test_render: $(LIB_SRC) $(TEST_RENDER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_RENDER_SRC) $(LDLIBS)

$(BUILD)/test_cache: $(LIB_SRC) $(TEST_CACHE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_CACHE_SRC) $(LDLIBS)

$(BUILD)/test_sanitize: $(LIB_SRC) $(TEST_SANITIZE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_SANITIZE_SRC) $(LDLIBS)

$(BUILD)/test_ws: $(LIB_SRC) $(TEST_WS_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_WS_SRC) $(LDLIBS)

$(BUILD)/test_interactive: $(LIB_SRC) $(TEST_INTERACTIVE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_INTERACTIVE_SRC) $(LDLIBS)

$(BUILD)/test_identify: $(LIB_SRC) $(TEST_IDENTIFY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_IDENTIFY_SRC) $(LDLIBS)

$(BUILD)/test_outbox: $(LIB_SRC) $(TEST_OUTBOX_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_OUTBOX_SRC) $(LDLIBS)

$(BUILD)/test_rtc: $(LIB_SRC) $(TEST_RTC_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_RTC_SRC) $(LDLIBS)

$(BUILD)/test_crawl: $(LIB_SRC) $(TEST_CRAWL_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_CRAWL_SRC) $(LDLIBS)

$(BUILD)/test_extract: $(LIB_SRC) $(TEST_EXTRACT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_EXTRACT_SRC) $(LDLIBS)

$(BUILD)/test_identity: $(LIB_SRC) $(TEST_IDENTITY_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_IDENTITY_SRC) $(LDLIBS)

$(BUILD)/test_captcha: $(LIB_SRC) $(TEST_CAPTCHA_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_CAPTCHA_SRC) $(LDLIBS)

$(BUILD)/test_frontier: $(LIB_SRC) $(TEST_FRONTIER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_FRONTIER_SRC) $(LDLIBS)

$(BUILD)/test_impersonate: $(LIB_SRC) $(TEST_IMPERSONATE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_IMPERSONATE_SRC) $(LDLIBS)

$(BUILD)/test_antibot: $(LIB_SRC) $(TEST_ANTIBOT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_ANTIBOT_SRC) $(LDLIBS)

$(BUILD)/test_browser_headers: $(LIB_SRC) $(TEST_BROWSER_HEADERS_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_BROWSER_HEADERS_SRC) $(LDLIBS)

$(BUILD)/test_politeness: $(LIB_SRC) $(TEST_POLITENESS_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_POLITENESS_SRC) $(LDLIBS)

$(BUILD)/test_condget: $(LIB_SRC) $(TEST_CONDGET_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_CONDGET_SRC) $(LDLIBS)

$(BUILD)/test_html: $(LIB_SRC) $(TEST_HTML_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_HTML_SRC) $(LDLIBS)

$(BUILD)/test_charset: $(LIB_SRC) $(TEST_CHARSET_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_CHARSET_SRC) $(LDLIBS)

$(BUILD)/test_structured: $(LIB_SRC) $(TEST_STRUCTURED_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_STRUCTURED_SRC) $(LDLIBS)

$(BUILD)/test_sitemap: $(LIB_SRC) $(TEST_SITEMAP_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_SITEMAP_SRC) $(LDLIBS)

$(BUILD)/test_fingerprint: $(LIB_SRC) $(TEST_FINGERPRINT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_FINGERPRINT_SRC) $(LDLIBS)

$(BUILD)/test_cookiejar: $(LIB_SRC) $(TEST_COOKIEJAR_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_COOKIEJAR_SRC) $(LDLIBS)

$(BUILD)/test_form: $(LIB_SRC) $(TEST_FORM_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_FORM_SRC) $(LDLIBS)

$(BUILD)/test_session: $(LIB_SRC) $(TEST_SESSION_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_SESSION_SRC) $(LDLIBS)

$(BUILD)/test_linkcheck: $(LIB_SRC) $(TEST_LINKCHECK_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_LINKCHECK_SRC) $(LDLIBS)

$(BUILD)/test_search: $(LIB_SRC) $(TEST_SEARCH_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_SEARCH_SRC) $(LDLIBS)

$(BUILD)/test_lang: $(LIB_SRC) $(TEST_LANG_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_LANG_SRC) $(LDLIBS)

$(BUILD)/test_captcha_inject: $(LIB_SRC) $(TEST_CAPTCHA_INJECT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_CAPTCHA_INJECT_SRC) $(LDLIBS)

$(BUILD)/test_html5: $(LIB_SRC) $(TEST_HTML5_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_HTML5_SRC) $(LDLIBS)

$(BUILD)/test_css: $(LIB_SRC) $(TEST_CSS_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_CSS_SRC) $(LDLIBS)

$(BUILD)/test_xpath: $(LIB_SRC) $(TEST_XPATH_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_XPATH_SRC) $(LDLIBS)

$(BUILD)/test_extract_rows: $(LIB_SRC) $(TEST_EXTRACT_ROWS_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_EXTRACT_ROWS_SRC) $(LDLIBS)


$(BUILD)/test_torrent: $(LIB_SRC) $(TEST_TORRENT_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIB_SRC) $(TEST_TORRENT_SRC) $(LDLIBS)

test: $(BUILD)/test_media_ingest $(BUILD)/test_server $(BUILD)/test_render $(BUILD)/test_cache $(BUILD)/test_sanitize $(BUILD)/test_ws $(BUILD)/test_interactive $(BUILD)/test_identify $(BUILD)/test_outbox $(BUILD)/test_rtc $(BUILD)/test_crawl $(BUILD)/test_extract $(BUILD)/test_identity $(BUILD)/test_captcha $(BUILD)/test_frontier $(BUILD)/test_impersonate $(BUILD)/test_antibot $(BUILD)/test_browser_headers $(BUILD)/test_politeness $(BUILD)/test_condget $(BUILD)/test_html $(BUILD)/test_charset $(BUILD)/test_structured $(BUILD)/test_sitemap $(BUILD)/test_fingerprint $(BUILD)/test_cookiejar $(BUILD)/test_form $(BUILD)/test_session $(BUILD)/test_linkcheck $(BUILD)/test_search $(BUILD)/test_lang $(BUILD)/test_captcha_inject $(BUILD)/test_html5 $(BUILD)/test_css $(BUILD)/test_xpath $(BUILD)/test_extract_rows $(BUILD)/test_torrent
	$(BUILD)/test_media_ingest
	$(BUILD)/test_server
	$(BUILD)/test_render
	$(BUILD)/test_cache
	$(BUILD)/test_sanitize
	$(BUILD)/test_ws
	$(BUILD)/test_interactive
	$(BUILD)/test_identify
	$(BUILD)/test_outbox
	$(BUILD)/test_rtc
	$(BUILD)/test_crawl
	$(BUILD)/test_extract
	$(BUILD)/test_identity
	$(BUILD)/test_captcha
	$(BUILD)/test_frontier
	$(BUILD)/test_impersonate
	$(BUILD)/test_antibot
	$(BUILD)/test_browser_headers
	$(BUILD)/test_politeness
	$(BUILD)/test_condget
	$(BUILD)/test_html
	$(BUILD)/test_charset
	$(BUILD)/test_structured
	$(BUILD)/test_sitemap
	$(BUILD)/test_fingerprint
	$(BUILD)/test_cookiejar
	$(BUILD)/test_form
	$(BUILD)/test_session
	$(BUILD)/test_linkcheck
	$(BUILD)/test_search
	$(BUILD)/test_lang
	$(BUILD)/test_captcha_inject
	$(BUILD)/test_html5
	$(BUILD)/test_css
	$(BUILD)/test_xpath
	$(BUILD)/test_extract_rows

clean:
	rm -rf $(BUILD)
