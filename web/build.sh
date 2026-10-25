#!/bin/sh
# Builds the gate for a web page: web/site/cywig.js + cywig.wasm, with
# Emscripten (emcc on the PATH: source emsdk_env.sh first). The page itself
# is web/site/ (index.html, app.js); serve that folder over https (or from
# localhost) in Chrome or Edge, which have WebUSB.
set -e
cd "$(dirname "$0")/.."
CYB=${CYB:-cybiko-browser}
[ -f "$CYB/gate/cy_gate.c" ] || { echo "the submodule is missing: git submodule update --init" >&2; exit 1; }
emcc -O2 -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-format-truncation \
    -I$CYB/include -I$CYB/src -I$CYB/third_party/tjpgd -I$CYB/gate -I$CYB/usb -Ipc -Ipc/cy_radio -Iweb \
    pc/cywig.c pc/relay.c pc/cy_radio/cy_radio.c web/usb_web.c web/net_web.c \
    $CYB/usb/cymcp.c $CYB/usb/cymcp_link.c \
    $CYB/src/cy_browser.c $CYB/src/cyb_cookie.c $CYB/src/cyb_css.c $CYB/src/cyb_font.c \
    $CYB/src/cyb_html.c $CYB/src/cyb_image.c $CYB/src/cyb_layout.c $CYB/src/cyb_text.c \
    $CYB/src/cyb_url.c $CYB/third_party/tjpgd/tjpgd.c \
    $CYB/gate/cy_deflate.c $CYB/gate/cy_gate.c $CYB/gate/cy_page.c $CYB/gate/cy_wml.c \
    -sASYNCIFY=1 -sMODULARIZE=1 -sEXPORT_NAME=CywigModule -sENVIRONMENT=worker \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=32MB -sSTACK_SIZE=1MB -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
    -sEXPORTED_FUNCTIONS=_main,_cywig_chat,_cywig_stop \
    -sEXPORTED_RUNTIME_METHODS=callMain,HEAPU8,HEAP32,UTF8ToString,stringToUTF8 \
    -o web/site/cywig.js
[ -f xtreme/USBGate.app ] && cp xtreme/USBGate.app web/site/ || echo "no xtreme/USBGate.app: build it (make -C xtreme) for the page's download link" >&2
ls -l web/site/cywig.js web/site/cywig.wasm
