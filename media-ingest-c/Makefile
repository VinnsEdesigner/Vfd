# media-ingest-c
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
           -Wmissing-prototypes -std=c11 -Isrc
LDFLAGS ?=
LDLIBS  := -lcurl -lpthread

SRC_DIR := src
BUILD   := build
BIN     := $(BUILD)/media-ingest
LIB_SRC := $(SRC_DIR)/mi_util.c $(SRC_DIR)/mi_detect.c \
           $(SRC_DIR)/mi_download.c $(SRC_DIR)/mi_hls.c \
           $(SRC_DIR)/mi_storage.c $(SRC_DIR)/mi_json.c \
           $(SRC_DIR)/mi_server.c
CLI_SRC := $(SRC_DIR)/mi_cli.c
TEST_SRC := tests/test_media_ingest.c
TEST_SERVER_SRC := tests/test_server.c

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

test: $(BUILD)/test_media_ingest $(BUILD)/test_server
	$(BUILD)/test_media_ingest
	$(BUILD)/test_server

clean:
	rm -rf $(BUILD)
