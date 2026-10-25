/* A small WebSocket client (RFC 6455) for the radio relay: ws:// and wss://
 * (OpenSSL), through an HTTP proxy when https_proxy / HTTPS_PROXY is set.
 * Binary messages only; pings are answered, everything else is logged. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct ws ws_t;

/* Connects and upgrades. origin may be NULL. NULL on failure, with why in err. */
ws_t *ws_open(const char *url, const char *origin, char *err, size_t errlen);
void ws_close(ws_t *w);

/* One binary message. 0 when sent, -1 when the connection is gone. */
int ws_send(ws_t *w, const void *data, size_t len);

/* Waits up to ms for a binary message. Returns its length (copied to buf,
 * cut to cap), 0 when none came, -1 when the connection is gone (why in
 * ws_error), -2 for a text message (its text in ws_error). */
int ws_recv(ws_t *w, uint8_t *buf, size_t cap, int ms);
const char *ws_error(const ws_t *w);
