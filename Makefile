CC       = clang
CFLAGS  ?= -O2 -g
ARCHS   ?= -arch arm64 -arch x86_64   # `make ARCHS=` builds for this Mac only
CFLAGS  += -std=gnu99 -Wall -Wextra -Wpedantic -Wno-gnu-zero-variadic-macro-arguments \
           -mmacosx-version-min=13.0 $(ARCHS)
LDFLAGS += -framework CoreFoundation -framework IOKit \
           -sectcreate __TEXT __info_plist Info.plist

BUILD    = build
BIN      = $(BUILD)/procon2-usb
SRCS     = src/main.c src/pc2_agent.c src/pc2_usb.c src/pc2_hid.c src/pc2_learn.c src/s2_proto.c
OBJS     = $(SRCS:src/%.c=$(BUILD)/%.o)

.PHONY: all clean test install uninstall

all: $(BIN)

$(BIN): $(OBJS) Info.plist
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	codesign --force --sign - --identifier io.github.procon2-usb $@

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test_s2_proto: tests/test_s2_proto.c src/s2_proto.c src/pc2_hid.c src/*.h | $(BUILD)
	$(CC) -std=gnu99 -Wall -Wextra -Isrc tests/test_s2_proto.c src/s2_proto.c src/pc2_hid.c \
	    -framework CoreFoundation -framework IOKit -o $@

test: $(BUILD)/test_s2_proto
	$(BUILD)/test_s2_proto

$(BUILD):
	mkdir -p $(BUILD)

install: $(BIN)
	scripts/install.sh

uninstall:
	scripts/uninstall.sh

clean:
	rm -rf $(BUILD)
