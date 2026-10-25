/* A small WebSocket client, see ws.h. */
#include "ws.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cy_sock.h"
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

struct ws {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    uint8_t in[70000];               /* what came and isn't a whole frame yet */
    size_t in_len;
    char error[200];
};

static void fail(ws_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(ws_t *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->error, sizeof w->error, fmt, ap);
    va_end(ap);
}

static int raw_write(ws_t *w, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        int r = w->ssl ? SSL_write(w->ssl, c, (int)n) : (int)send(w->fd, (const char *)c, (int)n, MSG_NOSIGNAL);
        if (r <= 0) {
            if (!w->ssl && errno == EINTR) continue;
            fail(w, "write failed");
            return -1;
        }
        c += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Reads what there is, waiting up to ms. 1 when something came, 0 when
 * nothing did, -1 when the connection is gone. */
static int raw_fill(ws_t *w, int ms)
{
    if (w->in_len == sizeof w->in) { fail(w, "frame too big"); return -1; }
    if (!(w->ssl && SSL_pending(w->ssl))) {
        struct pollfd p = { w->fd, POLLIN, 0 };
        int r = cy_poll(&p, 1, ms);
        if (r == 0) return 0;
        if (r < 0) return errno == EINTR ? 0 : -1;
    }
    int r = w->ssl ? SSL_read(w->ssl, w->in + w->in_len, (int)(sizeof w->in - w->in_len))
                   : (int)recv(w->fd, (char *)w->in + w->in_len, (int)(sizeof w->in - w->in_len), 0);
    if (r <= 0) {
        if (w->ssl && SSL_get_error(w->ssl, r) == SSL_ERROR_WANT_READ) return 0;
        fail(w, "the relay closed the connection");
        return -1;
    }
    w->in_len += (size_t)r;
    return 1;
}

/* A header line block, up to the empty line. */
static int read_head(ws_t *w, char *out, size_t cap)
{
    for (;;) {
        for (size_t i = 3; i < w->in_len; i++)
            if (!memcmp(w->in + i - 3, "\r\n\r\n", 4)) {
                size_t n = i + 1 < cap ? i + 1 : cap - 1;
                memcpy(out, w->in, n);
                out[n] = 0;
                memmove(w->in, w->in + i + 1, w->in_len - i - 1);
                w->in_len -= i + 1;
                return 0;
            }
        int r = raw_fill(w, 15000);
        if (r <= 0) { if (r == 0) fail(w, "no answer from the relay"); return -1; }
    }
}

static int dial(ws_t *w, const char *host, const char *port)
{
    struct addrinfo hints = { 0 }, *res, *a;
    cy_sock_init();
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host, port, &hints, &res);
    if (e) { fail(w, "%s: %s", host, gai_strerror(e)); return -1; }
    for (a = res; a; a = a->ai_next) {
        w->fd = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (w->fd < 0) continue;
        if (!connect(w->fd, a->ai_addr, a->ai_addrlen)) break;
        cy_sock_close(w->fd);
        w->fd = -1;
    }
    freeaddrinfo(res);
    if (w->fd < 0) { fail(w, "can't connect to %s port %s", host, port); return -1; }
    return 0;
}

/* scheme://host[:port]/path */
static int split(const char *url, int *tls, char *host, size_t hl, char *port, size_t pl, char *path, size_t pathl)
{
    const char *p;
    if (!strncasecmp(url, "wss://", 6) || !strncasecmp(url, "https://", 8)) *tls = 1;
    else if (!strncasecmp(url, "ws://", 5) || !strncasecmp(url, "http://", 7)) *tls = 0;
    else return -1;
    p = strstr(url, "://") + 3;
    size_t n = strcspn(p, ":/");
    if (!n || n >= hl) return -1;
    memcpy(host, p, n);
    host[n] = 0;
    p += n;
    if (*p == ':') {
        p++;
        n = strcspn(p, "/");
        if (!n || n >= pl) return -1;
        memcpy(port, p, n);
        port[n] = 0;
        p += n;
    } else {
        snprintf(port, pl, "%s", *tls ? "443" : "80");
    }
    snprintf(path, pathl, "%s", *p ? p : "/");
    return 0;
}

ws_t *ws_open(const char *url, const char *origin, char *err, size_t errlen)
{
    ws_t *w = calloc(1, sizeof *w);
    char host[256], port[16], path[512], head[4096];
    int tls;
    if (!w) { snprintf(err, errlen, "out of memory"); return NULL; }
    w->fd = -1;
    if (split(url, &tls, host, sizeof host, port, sizeof port, path, sizeof path) < 0) {
        snprintf(err, errlen, "%s: not a ws:// or wss:// address", url);
        free(w);
        return NULL;
    }
    /* through a proxy when one is set (wss only: a CONNECT tunnel) */
    const char *proxy = tls ? getenv("https_proxy") : NULL;
    if (tls && !proxy) proxy = getenv("HTTPS_PROXY");
    if (proxy && *proxy) {
        int ptls;
        char ph[256], pp[16], ppath[64];
        if (split(proxy, &ptls, ph, sizeof ph, pp, sizeof pp, ppath, sizeof ppath) < 0 || ptls) {
            snprintf(err, errlen, "proxy %s: only http:// proxies", proxy);
            free(w);
            return NULL;
        }
        if (dial(w, ph, pp) < 0) goto bad;
        snprintf(head, sizeof head, "CONNECT %s:%s HTTP/1.1\r\nHost: %s:%s\r\n\r\n", host, port, host, port);
        if (raw_write(w, head, strlen(head)) < 0 || read_head(w, head, sizeof head) < 0) goto bad;
        if (strncmp(head, "HTTP/1.1 200", 12) && strncmp(head, "HTTP/1.0 200", 12)) {
            fail(w, "proxy said: %.*s", (int)strcspn(head, "\r\n"), head);
            goto bad;
        }
    } else if (dial(w, host, port) < 0) {
        goto bad;
    }
    if (tls) {
        w->ctx = SSL_CTX_new(TLS_client_method());
        if (!w->ctx) { fail(w, "TLS setup failed"); goto bad; }
        SSL_CTX_set_default_verify_paths(w->ctx);
#if defined _WIN32 && OPENSSL_VERSION_NUMBER >= 0x30200000L
        SSL_CTX_load_verify_store(w->ctx, "org.openssl.winstore:");   /* Windows' own certificates */
#endif
        const char *ca = getenv("SSL_CERT_FILE");
        if (ca) SSL_CTX_load_verify_locations(w->ctx, ca, NULL);
        SSL_CTX_set_verify(w->ctx, SSL_VERIFY_PEER, NULL);
        w->ssl = SSL_new(w->ctx);
        SSL_set_tlsext_host_name(w->ssl, host);
        SSL_set1_host(w->ssl, host);
        SSL_set_fd(w->ssl, w->fd);
        if (SSL_connect(w->ssl) != 1) {
            char e[120];
            ERR_error_string_n(ERR_get_error(), e, sizeof e);
            fail(w, "TLS with %s failed: %s", host, e);
            goto bad;
        }
    }
    uint8_t key[16];
    unsigned char key64[32];
    RAND_bytes(key, sizeof key);
    EVP_EncodeBlock(key64, key, sizeof key);
    int n = snprintf(head, sizeof head,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: cywig\r\n",
                     path, host, key64);
    if (origin) n += snprintf(head + n, sizeof head - (size_t)n, "Origin: %s\r\n", origin);
    snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (raw_write(w, head, strlen(head)) < 0 || read_head(w, head, sizeof head) < 0) goto bad;
    if (strncmp(head + 8, " 101", 4)) {
        fail(w, "the relay said: %.*s", (int)strcspn(head, "\r\n"), head);
        goto bad;
    }
    return w;
bad:
    snprintf(err, errlen, "%s", w->error);
    ws_close(w);
    return NULL;
}

void ws_close(ws_t *w)
{
    if (!w) return;
    if (w->ssl) { SSL_shutdown(w->ssl); SSL_free(w->ssl); }
    if (w->ctx) SSL_CTX_free(w->ctx);
    if (w->fd >= 0) cy_sock_close(w->fd);
    free(w);
}

static int send_frame(ws_t *w, int op, const void *data, size_t len)
{
    uint8_t h[14], mask[4];
    size_t hl = 2;
    h[0] = (uint8_t)(0x80 | op);
    if (len < 126) {
        h[1] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        h[1] = 0x80 | 126;
        h[2] = (uint8_t)(len >> 8);
        h[3] = (uint8_t)len;
        hl = 4;
    } else {
        fail(w, "message too big");
        return -1;
    }
    RAND_bytes(mask, 4);
    memcpy(h + hl, mask, 4);
    hl += 4;
    uint8_t b[16 + 65536];
    memcpy(b, h, hl);
    for (size_t i = 0; i < len; i++) b[hl + i] = ((const uint8_t *)data)[i] ^ mask[i & 3];
    return raw_write(w, b, hl + len);
}

int ws_send(ws_t *w, const void *data, size_t len) { return send_frame(w, 2, data, len); }

int ws_recv(ws_t *w, uint8_t *buf, size_t cap, int ms)
{
    for (;;) {
        /* a whole frame in? */
        if (w->in_len >= 2) {
            size_t hl = 2, len = w->in[1] & 0x7f;
            int op = w->in[0] & 0x0f, masked = w->in[1] & 0x80;
            if (len == 126) {
                hl = 4;
                if (w->in_len >= 4) len = (size_t)w->in[2] << 8 | w->in[3];
            } else if (len == 127) {
                fail(w, "frame too big");
                return -1;
            }
            if (masked) hl += 4;
            if (w->in_len >= hl && w->in_len >= hl + len) {
                uint8_t *d = w->in + hl;
                if (masked)
                    for (size_t i = 0; i < len; i++) d[i] ^= w->in[hl - 4 + (i & 3)];
                int result = 0;
                if (op == 2 || op == 0) {
                    size_t n = len < cap ? len : cap;
                    memcpy(buf, d, n);
                    result = (int)n;
                } else if (op == 9) {
                    send_frame(w, 10, d, len);          /* pong */
                } else if (op == 8) {
                    fail(w, "the relay closed the connection (%u)", len >= 2 ? (unsigned)(d[0] << 8 | d[1]) : 0u);
                    return -1;
                } else if (op == 1) {
                    fail(w, "relay says: %.*s", (int)(len < 150 ? len : 150), (const char *)d);
                    result = -2;                        /* text: reported, not a packet */
                }
                memmove(w->in, w->in + hl + len, w->in_len - hl - len);
                w->in_len -= hl + len;
                if (result) return result;
                continue;
            }
        }
        int r = raw_fill(w, ms);
        if (r <= 0) return r;
        ms = 0;
    }
}

const char *ws_error(const ws_t *w) { return w->error; }
