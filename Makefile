# The PC tool (pc/cywig) and the Xtreme application (xtreme/USBGate.app).
# make            the PC tool, with libusb if pkg-config finds it
# make NET=libcurl  fetch with libcurl instead of running curl (the default on Windows)
# make xtreme     the Xtreme application (needs the GNU H8 tools)
CC ?= cc
CYB ?= cybiko-browser
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-format-truncation \
	-I$(CYB)/include -I$(CYB)/src -I$(CYB)/third_party/tjpgd -I$(CYB)/host -I$(CYB)/gate -I$(CYB)/usb -Ipc -Ipc/cy_radio
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0 2>/dev/null)
ifneq ($(LIBUSB_LIBS),)
CFLAGS += -DCYWIG_LIBUSB $(LIBUSB_CFLAGS)
USB_SRC = $(CYB)/usb/cyusb_libusb.c
endif
# Windows (MSYS2 MinGW): libcurl instead of the curl program, Winsock
ifeq ($(OS),Windows_NT)
NET ?= libcurl
EXE = .exe
SYS_LIBS = -lws2_32 -lbcrypt
CFLAGS += -D__USE_MINGW_ANSI_STDIO=1 -include pc/win_compat.h
WIN_SRC = pc/win_compat.c
endif
ifeq ($(NET),libcurl)
CFLAGS += -DCYWIG_LIBCURL $(shell pkg-config --cflags libcurl 2>/dev/null)
NET_SRC = $(CYB)/host/cyb_net_libcurl.c
NET_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
else
NET_SRC = $(CYB)/host/cyb_net_curl.c
endif
SRC = $(WIN_SRC) pc/cywig.c pc/relay.c pc/ws.c pc/cy_radio/cy_radio.c $(CYB)/usb/cymcp.c $(CYB)/usb/cymcp_link.c $(CYB)/usb/cyusb_tcp.c $(USB_SRC) \
	$(NET_SRC) \
	$(CYB)/src/cy_browser.c $(CYB)/src/cyb_cookie.c $(CYB)/src/cyb_css.c $(CYB)/src/cyb_font.c \
	$(CYB)/src/cyb_html.c $(CYB)/src/cyb_image.c $(CYB)/src/cyb_layout.c $(CYB)/src/cyb_text.c \
	$(CYB)/src/cyb_url.c $(CYB)/third_party/tjpgd/tjpgd.c \
	$(CYB)/gate/cy_deflate.c $(CYB)/gate/cy_gate.c $(CYB)/gate/cy_page.c $(CYB)/gate/cy_wml.c

all: build/cywig$(EXE) build/chat_peer$(EXE)

build/cywig$(EXE): $(SRC) xtreme/proto.h pc/relay.h pc/ws.h pc/cy_radio/cy_radio.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LIBUSB_LIBS) $(NET_LIBS) -lssl -lcrypto -lpthread -lm $(SYS_LIBS)

build/chat_peer$(EXE): tools/chat_peer.c pc/relay.c pc/ws.c pc/cy_radio/cy_radio.c pc/relay.h pc/ws.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(WIN_SRC) tools/chat_peer.c pc/relay.c pc/ws.c pc/cy_radio/cy_radio.c -o $@ -lssl -lcrypto -lpthread $(SYS_LIBS)

xtreme:
	sh xtreme/build.sh

clean:
	rm -rf build

.PHONY: all xtreme clean
