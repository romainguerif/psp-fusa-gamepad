# PSP Bridge — Mac tools: pspbridge (step 1 tests), pspbridged (ISO server). Needs Homebrew libusb and pkg-config.
#   make          build/pspbridge
#   make test     offline tests (fake PSP, no cable)
CC      ?= clang
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
USB_LIBS   := $(shell pkg-config --libs libusb-1.0)

BUILD  := build
PROTO  := ../src/common/bridge_proto.c
COMMON := bridge_client.c fake_psp.c $(PROTO)
HDRS   := bridge_client.h fake_psp.h bridge_usb.h ../src/common/bridge_proto.h

all: $(BUILD)/pspbridge $(BUILD)/pspbridged

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/pspbridge: pspbridge.c bridge_usb.c $(COMMON) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(USB_CFLAGS) pspbridge.c bridge_usb.c $(COMMON) -o $@ \
		$(USB_LIBS) -framework IOKit -framework CoreFoundation -lpthread

$(BUILD)/pspbridged: pspbridged.c bridge_usb.c iso_info.c iso_info.h pad_forward.c pad_forward.h $(COMMON) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(USB_CFLAGS) pspbridged.c bridge_usb.c iso_info.c pad_forward.c $(COMMON) -o $@ \
		$(USB_LIBS) -framework IOKit -framework CoreFoundation -lpthread

$(BUILD)/test_pad: test_pad.c pad_forward.c pad_forward.h | $(BUILD)
	$(CC) $(CFLAGS) $(USB_CFLAGS) test_pad.c pad_forward.c -o $@ -framework IOKit -framework CoreFoundation

$(BUILD)/test_bridge: test_bridge.c $(COMMON) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined test_bridge.c $(COMMON) -o $@

test: $(BUILD)/test_bridge $(BUILD)/test_pad $(BUILD)/pspbridge $(BUILD)/pspbridged
	$(BUILD)/test_bridge
	$(BUILD)/test_pad
	/opt/homebrew/bin/python3 test_http.py
	$(BUILD)/pspbridge --fake hello
	$(BUILD)/pspbridge --fake echo 500

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
