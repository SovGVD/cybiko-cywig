/* The Xtreme's USB in a web page: cyusb_dev_t over Module.usb in
 * web/site/gate-worker.js (WebUSB, or a WebSocket to Dragonfruit's emulator). The
 * transfers are the page's promises; Asyncify lets the C wait for them as
 * if they were blocking calls. */
#include <emscripten.h>
#include <stdio.h>
#include <string.h>
#include "cyusb.h"

EM_ASYNC_JS(int, js_usb_control, (int type, int req, int value, int index, uint8_t *data, int len, int timeout), {
    const out = type & 0x80 ? null : HEAPU8.slice(data, data + len);
    const r = await Module.usb.control(type, req, value, index, len, out, timeout);
    if (r.data) HEAPU8.set(r.data.subarray(0, Math.min(len, r.data.length)), data);
    return r.result;
});

EM_ASYNC_JS(int, js_usb_bulk, (int ep, uint8_t *data, int len, int timeout, int *timed_out), {
    const out = ep & 0x80 ? null : HEAPU8.slice(data, data + len);
    const r = await Module.usb.bulk(ep, len, out, timeout);
    if (r.data) HEAPU8.set(r.data.subarray(0, Math.min(len, r.data.length)), data);
    HEAP32[timed_out >> 2] = r.timedOut ? 1 : 0;
    return r.result;
});

EM_JS(void, js_usb_log, (const char *s), { Module.log("usb: " + UTF8ToString(s)); });

static int control(cyusb_dev_t *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint8_t *data,
                   uint16_t len, unsigned timeout_ms)
{
    (void)d;
    return js_usb_control(type, req, value, index, data, len, (int)timeout_ms);
}

static int bulk(cyusb_dev_t *d, uint8_t ep, uint8_t *data, int len, unsigned timeout_ms, int *timed_out)
{
    (void)d;
    *timed_out = 0;
    return js_usb_bulk(ep, data, len, (int)timeout_ms, timed_out);
}

static void log_(cyusb_dev_t *d, const char *msg) { (void)d; js_usb_log(msg); }
static unsigned now_ms(cyusb_dev_t *d) { (void)d; return (unsigned)emscripten_get_now(); }

static cyusb_dev_t dev = { control, bulk, log_, now_ms, NULL };

/* The page's cable (Module.usb.open): waits until there is one, again
 * after the Xtreme left the bus; 0 when the page is stopping the gate. */
EM_ASYNC_JS(int, js_usb_open, (void), { return (await Module.usb.open()) ? 1 : 0; });

cyusb_dev_t *cyusb_web_open(void) { return js_usb_open() ? &dev : NULL; }
