/* Chat over the radio relay, see relay.h. On a PC a thread of its own
 * talks to the relay (ws.c); in a web page (Emscripten) the page's
 * WebSocket does (Module.relay in web/app.js) and relay_poll does the
 * rest. */
#include "relay.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "compat.h"
#include "cy_radio.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
typedef int pthread_mutex_t;
#define pthread_mutex_lock(m) ((void)0)
#define pthread_mutex_unlock(m) ((void)0)
#define pthread_mutex_init(m, a) ((void)0)
#define pthread_mutex_destroy(m) ((void)0)
EM_JS(void, js_relay_open, (const char *url), { Module.relay.open(UTF8ToString(url)); });
EM_JS(void, js_relay_close, (void), { Module.relay.close(); });
EM_JS(int, js_relay_online, (void), { return Module.relay.online() ? 1 : 0; });
EM_JS(void, js_relay_send, (const uint8_t *p, int n), { Module.relay.send(HEAPU8.slice(p, p + n)); });
/* the next message from the room: its length, 0 when none, -2 for a text
 * one (its text in the buffer) */
EM_JS(int, js_relay_next, (uint8_t *p, int cap), {
    const m = Module.relay.next();
    if (!m) return 0;
    if (typeof m === "string") { stringToUTF8(m, p, cap); return -2; }
    const n = Math.min(m.length, cap);
    HEAPU8.set(m.subarray(0, n), p);
    return n;
});
#else
#include <pthread.h>
#include "ws.h"
#endif

#define PING_MS 5000
#define COPIES 3                /* a broadcast line goes three times */
#define COPY_MS 250
#define OUT_MAX 64
#define IN_MAX 32
#define NICKS 64
#define SEEN 64

typedef struct {
    uint32_t from;
    int room;
    char nick[9];
    char text[CYR_CHAT_MAX + 1];
} heard_t;

struct relay {
    char url[600];
    char origin[200];
    uint32_t id;
    char nick[9];
    uint8_t version;
    relay_host_t host;

#ifndef __EMSCRIPTEN__
    pthread_t thread;
    ws_t *ws;
#endif
    pthread_mutex_t mu;
    volatile int quit;
    volatile int online;
    uint32_t next_ping;
    uint32_t tab, seq;

    /* to send: packets and when */
    struct { uint8_t pkt[CYR_LONG]; uint8_t len, channel; uint32_t at; } out[OUT_MAX];
    int nout;
    /* heard, for relay_poll */
    heard_t in[IN_MAX];
    int nin;
    unsigned in_lost;

    /* the thread's own */
    struct { uint32_t id; char nick[9]; } nicks[NICKS];
    int nnicks;
    struct { uint32_t from, p0; } seen[SEEN];
    int seen_at;
    cyr_rx_t rx;
};

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void logf_r(relay_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_r(relay_t *r, const char *fmt, ...)
{
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (r->host.log) r->host.log(r->host.user, b);
}

/* Queue a packet (locked by the caller). */
static void queue(relay_t *r, const uint8_t *pkt, size_t len, uint8_t channel, uint32_t at)
{
    if (r->nout == OUT_MAX) {
        logf_r(r, "relay: too much to send, a packet dropped");
        return;
    }
    memcpy(r->out[r->nout].pkt, pkt, len);
    r->out[r->nout].len = (uint8_t)len;
    r->out[r->nout].channel = channel;
    r->out[r->nout].at = at;
    r->nout++;
}

static void send_now(relay_t *r, const uint8_t *pkt, size_t len, uint8_t channel)
{
    uint8_t f[9 + CYR_LONG];
    f[0] = (uint8_t)(r->tab >> 24); f[1] = (uint8_t)(r->tab >> 16); f[2] = (uint8_t)(r->tab >> 8); f[3] = (uint8_t)r->tab;
    f[4] = (uint8_t)(r->seq >> 24); f[5] = (uint8_t)(r->seq >> 16); f[6] = (uint8_t)(r->seq >> 8); f[7] = (uint8_t)r->seq;
    r->seq++;
    f[8] = channel;
    memcpy(f + 9, pkt, len);
#ifdef __EMSCRIPTEN__
    if (js_relay_online()) js_relay_send(f, (int)(9 + len));
#else
    if (r->ws && ws_send(r->ws, f, 9 + len) < 0) logf_r(r, "relay: send failed: %s", ws_error(r->ws));
#endif
}

static const char *nick_of(relay_t *r, uint32_t id)
{
    for (int i = 0; i < r->nnicks; i++)
        if (r->nicks[i].id == id) return r->nicks[i].nick;
    return "";
}

static void set_nick(relay_t *r, uint32_t id, const char *nick)
{
    int i;
    for (i = 0; i < r->nnicks && r->nicks[i].id != id; i++) {}
    if (i == r->nnicks) {
        if (r->nnicks == NICKS) i = (int)(id % NICKS);   /* full: take a place */
        else r->nnicks++;
        r->nicks[i].id = id;
        r->nicks[i].nick[0] = 0;
    }
    if (strcmp(r->nicks[i].nick, nick)) {
        char s[8];
        cyr_cyid_str(id, s);
        logf_r(r, "relay: %s is %s", s, nick);
        snprintf(r->nicks[i].nick, sizeof r->nicks[i].nick, "%s", nick);
    }
}

/* A line once: the copies have the same sender and clock. */
static bool first_time(relay_t *r, uint32_t from, uint32_t p0)
{
    for (int i = 0; i < SEEN; i++)
        if (r->seen[i].from == from && r->seen[i].p0 == p0) return false;
    r->seen[r->seen_at].from = from;
    r->seen[r->seen_at].p0 = p0;
    r->seen_at = (r->seen_at + 1) % SEEN;
    return true;
}

static void packet(relay_t *r, const uint8_t *pkt, size_t len, uint8_t channel)
{
    if (!cyr_check(pkt, len)) {
        logf_r(r, "relay: a packet with a bad CRC from the room; ignored");
        return;
    }
    const uint32_t to = cyr_to(pkt), from = cyr_from(pkt);
    const uint8_t type = cyr_type(pkt);
    if (type == CYR_T_PING) {
        char nick[9];
        int age;
        bool female;
        if (cyr_ping_details(pkt, nick, &age, &female)) set_nick(r, from, nick);
        return;
    }
    if (to != r->id && to != CYR_BROADCAST) return;    /* between others */
    uint8_t ack[CYR_SHORT];
    if (type == CYR_T_QUERY) {
        pthread_mutex_lock(&r->mu);
        send_now(r, ack, cyr_ack(ack, pkt, r->id, channel, CYR_ACK_DELIVERED), channel);
        pthread_mutex_unlock(&r->mu);
        return;
    }
    if (type != CYR_T_MSG) return;                       /* acks: we wait for none */
    uint8_t body[CYR_MSG_MAX];
    int n = cyr_rx_feed(&r->rx, pkt, len, body);
    if (to == r->id && (pkt[9] & CYR_F_ACK_ME)) {
        pthread_mutex_lock(&r->mu);
        send_now(r, ack, cyr_ack(ack, pkt, r->id, channel, pkt[10] && n == 0 ? CYR_ACK_TAKEN : CYR_ACK_DELIVERED),
                 channel);
        pthread_mutex_unlock(&r->mu);
    }
    if (n < 0) { logf_r(r, "relay: part %u from %08x out of order; dropped", pkt[10], (unsigned)from); return; }
    if (n == 0) return;
    cyr_msg_t m;
    int room;
    const char *text;
    size_t tl;
    char s[8];
    cyr_cyid_str(from, s);
    if (!cyr_msg_parse(body, (size_t)n, &m)) { logf_r(r, "relay: a message from %s we can't read; ignored", s); return; }
    if (!cyr_chat_parse(&m, &room, &text, &tl)) {
        logf_r(r, "relay: message %04x from %s (%u bytes) is not a chat line; not passed on", m.id, s, (unsigned)m.len);
        return;
    }
    if (!first_time(r, from, m.p0)) return;              /* a copy */
    if (to == r->id || (m.extra & 0x80)) {
        logf_r(r, "relay: private line from %s to the bridge: \"%.*s\"; not passed on", s, (int)tl, text);
        return;
    }
    pthread_mutex_lock(&r->mu);
    if (r->nin == IN_MAX) {
        r->in_lost++;
    } else {
        heard_t *h = &r->in[r->nin++];
        h->from = from;
        h->room = room;
        snprintf(h->nick, sizeof h->nick, "%s", nick_of(r, from));
        snprintf(h->text, sizeof h->text, "%.*s", (int)tl, text);
    }
    pthread_mutex_unlock(&r->mu);
}

/* Pings and what is due to go out (locked by the caller). */
static void tick(relay_t *r, uint32_t t)
{
    if ((int32_t)(t - r->next_ping) >= 0) {
        uint8_t p[CYR_SHORT];
        r->next_ping = t + PING_MS;
        send_now(r, p, cyr_ping(p, r->id, CYR_RF_CHAT, r->version, r->nick, -1, false), CYR_RF_BASE);
        send_now(r, p, cyr_ping(p, r->id, CYR_RF_CHAT, r->version, r->nick, -1, false), CYR_RF_CHAT);
    }
    for (int i = 0; i < r->nout;) {
        if ((int32_t)(t - r->out[i].at) >= 0) {
            send_now(r, r->out[i].pkt, r->out[i].len, r->out[i].channel);
            r->out[i] = r->out[--r->nout];
        } else {
            i++;
        }
    }
}

static void frame_in(relay_t *r, const uint8_t *f, int n)
{
    if (n == 9 + CYR_SHORT || n == 9 + CYR_LONG) packet(r, f + 9, (size_t)n - 9, f[8]);
    else logf_r(r, "relay: a %d-byte message from the room is not a packet; ignored", n);
}

#ifndef __EMSCRIPTEN__
static void *run(void *arg)
{
    relay_t *r = arg;
    uint32_t retry_at = 0, wait = 1000;
    static uint8_t f[70000];
    while (!r->quit) {
        uint32_t t = now_ms();
        if (!r->ws) {
            if ((int32_t)(t - retry_at) < 0) {
                struct timespec ts = { 0, 100 * 1000000 };
                nanosleep(&ts, NULL);
                continue;
            }
            char err[256];
            ws_t *w = ws_open(r->url, r->origin[0] ? r->origin : NULL, err, sizeof err);
            if (!w) {
                logf_r(r, "relay: %s: %s; again in %u s", r->url, err, (unsigned)(wait / 1000));
                retry_at = now_ms() + wait;
                wait = wait < 30000 ? wait * 2 : 30000;
                continue;
            }
            pthread_mutex_lock(&r->mu);
            r->ws = w;
            pthread_mutex_unlock(&r->mu);
            r->online = 1;
            wait = 1000;
            r->next_ping = t;
            logf_r(r, "relay: joined %s", r->url);
        }
        int n = ws_recv(r->ws, f, sizeof f, 50);
        if (n == -2) {
            logf_r(r, "relay: %s", ws_error(r->ws));
        } else if (n < 0) {
            logf_r(r, "relay: %s; joining again in %u s", ws_error(r->ws), (unsigned)(wait / 1000));
            pthread_mutex_lock(&r->mu);
            ws_close(r->ws);
            r->ws = NULL;
            pthread_mutex_unlock(&r->mu);
            r->online = 0;
            retry_at = now_ms() + wait;
            continue;
        } else if (n > 0) {
            frame_in(r, f, n);
        }
        pthread_mutex_lock(&r->mu);
        tick(r, now_ms());
        pthread_mutex_unlock(&r->mu);
    }
    return NULL;
}
#endif

relay_t *relay_create(const char *url, const char *room, const char *origin, uint32_t my_id, const char *nick,
                      const relay_host_t *host)
{
    relay_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    const char *rest = strstr(url, "://");
    size_t n = strlen(url);
    while (n && url[n - 1] == '/') n--;
    snprintf(r->url, sizeof r->url, "%s%.*s/room/%s", !strncmp(url, "https", 5) || !strncmp(url, "wss", 3) ? "wss" : "ws",
             rest ? (int)(n - (size_t)(rest - url)) : (int)n, rest ? rest : url, room);
    snprintf(r->origin, sizeof r->origin, "%s", origin ? origin : "");
    r->id = my_id;
    snprintf(r->nick, sizeof r->nick, "%s", nick);
    if (getentropy(&r->version, 1) || getentropy(&r->tab, sizeof r->tab)) { r->version = (uint8_t)time(NULL); r->tab = (uint32_t)time(NULL); }
    if (!r->version) r->version = 1;
    r->host = *host;
    pthread_mutex_init(&r->mu, NULL);
#ifdef __EMSCRIPTEN__
    js_relay_open(r->url);
#else
    if (pthread_create(&r->thread, NULL, run, r)) { free(r); return NULL; }
#endif
    return r;
}

void relay_destroy(relay_t *r)
{
    if (!r) return;
    r->quit = 1;
#ifdef __EMSCRIPTEN__
    js_relay_close();
#else
    pthread_join(r->thread, NULL);
    if (r->ws) ws_close(r->ws);
#endif
    pthread_mutex_destroy(&r->mu);
    free(r);
}

bool relay_online(const relay_t *r)
{
#ifdef __EMSCRIPTEN__
    return r && js_relay_online();
#else
    return r && r->online;
#endif
}

void relay_poll(relay_t *r)
{
#ifdef __EMSCRIPTEN__
    /* the page's WebSocket does the talking; the rest happens here */
    static uint8_t f[9 + CYR_LONG + 64];
    int fl;
    int online = js_relay_online();
    if (online && !r->online) { logf_r(r, "relay: joined %s", r->url); r->next_ping = now_ms(); }
    r->online = online;
    while ((fl = js_relay_next(f, sizeof f - 1)) != 0) {
        if (fl == -2) logf_r(r, "relay says: %s", (const char *)f);
        else frame_in(r, f, fl);
    }
    if (online) tick(r, now_ms());
#endif
    heard_t got[IN_MAX];
    int n;
    unsigned lost;
    pthread_mutex_lock(&r->mu);
    n = r->nin;
    memcpy(got, r->in, sizeof got[0] * (size_t)n);
    r->nin = 0;
    lost = r->in_lost;
    r->in_lost = 0;
    pthread_mutex_unlock(&r->mu);
    if (lost) logf_r(r, "relay: %u lines came faster than they could be passed on; dropped", lost);
    for (int i = 0; i < n; i++)
        if (r->host.line) r->host.line(r->host.user, got[i].from, got[i].nick, got[i].room, got[i].text);
}

void relay_say(relay_t *r, uint32_t from, int room, uint32_t clock, const char *text)
{
    cyr_msg_t m;
    uint8_t pk[2][CYR_LONG];
    cyr_chat(&m, room, clock, text, false);
    int n = cyr_msg_packets(&m, CYR_BROADCAST, from, CYR_RF_CHAT, false, 0, pk, 2);
    if (!n) {
        logf_r(r, "relay: line too long for the radio: \"%s\"", text);
        return;
    }
    uint32_t t = now_ms();
    pthread_mutex_lock(&r->mu);
    for (int c = 0; c < COPIES; c++)
        for (int i = 0; i < n; i++) queue(r, pk[i], CYR_LONG, CYR_RF_CHAT, t + (uint32_t)(c * COPY_MS + i * 50));
    pthread_mutex_unlock(&r->mu);
}
