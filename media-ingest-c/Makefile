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
LDLIBS  := -lcurl -lpthread $(GST_LIBS)

SRC_DIR := src
BUILD   := build
BIN     := $(BUILD)/media-ingest
LIB_SRC := $(SRC_DIR)/mi_util.c $(SRC_DIR)/mi_detect.c \
           $(SRC_DIR)/mi_download.c $(SRC_DIR)/mi_hls.c \
           $(SRC_DIR)/mi_storage.c $(SRC_DIR)/mi_json.c \
           $(SRC_DIR)/mi_browse.c $(SRC_DIR)/mi_render.c $(SRC_DIR)/mi_cache.c \
           $(SRC_DIR)/mi_sanitize.c $(SRC_DIR)/mi_identify.c $(SRC_DIR)/mi_outbox.c $(SRC_DIR)/mi_rtc.c $(SRC_DIR)/mi_ws.c $(SRC_DIR)/mi_interactive.c \
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

test: $(BUILD)/test_media_ingest $(BUILD)/test_server $(BUILD)/test_render $(BUILD)/test_cache $(BUILD)/test_sanitize $(BUILD)/test_ws $(BUILD)/test_interactive $(BUILD)/test_identify $(BUILD)/test_outbox $(BUILD)/test_rtc
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

clean:
	rm -rf $(BUILD)
