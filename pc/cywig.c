/* cywig: the PC half of a Cybiko Internet Gate (CyWIG) made of a stock
 * Cybiko Xtreme on the USB cable.
 *
 * The Xtreme runs the USB Gate application (xtreme/), which makes it the
 * gate for the Cybikos around it and passes the messages they send to the
 * gate's port 93 over the cable. This program takes them, fetches the pages
 * with cybiko-browser's gate code (WML for the Xtreme's WAP browser, laid-out
 * pages for Browser.app) and sends the answers back to be delivered over the
 * radio. Nothing on the Xtreme is changed: the cable carries CyOS's own
 * Multi Channel Protocol (cybiko-browser usb/cymcp.h), and this program
 * starts the application through CyOS's console.
 *
 * With --chat it also bridges Chat to the Internet: the Xtreme hands over
 * the chat lines it hears and this program passes them to a room of the
 * radio relay that the Cybiko web emulator uses (relay.h), and the room's
 * lines back to the Cybikos around. */

#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <unistd.h>
#include <signal.h>
#include "compat.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cymcp.h"
#ifdef __EMSCRIPTEN__
/* in a web page (web/): the page's USB and fetch */
#include <emscripten.h>
#include "net_web.h"
cyusb_dev_t *cyusb_web_open(void);
#else
#include "cyusb_tcp.h"
#endif
#ifdef CYWIG_LIBUSB
#include "cyusb_libusb.h"
#endif
#include "cy_gate.h"
#ifndef __EMSCRIPTEN__
#ifdef CYWIG_LIBCURL
#include "cyb_net_libcurl.h"
#else
#include "cyb_net_curl.h"
#endif
#endif
#include "../xtreme/proto.h"
#include "cy_radio.h"
#include "relay.h"

#define RELAY_URL "https://cybiko-relay.sovgvd.workers.dev"
#define RELAY_ORIGIN "https://sovgvd.github.io"   /* the web emulator's site */
#define CHAT_TAG 0x8000                           /* UG_SEND tags of chat lines */

#define PING_MS 3000
#define APP_GONE_MS 15000
#define START_TRIES 3
#define APP_NAME "USBGate.app"

static volatile sig_atomic_t stop;
static int verbose;

#ifdef __MINGW32__
#define LOG_FORMAT gnu_printf          /* MinGW's printf is told to be the C99 one */
#else
#define LOG_FORMAT printf
#endif
static void logf_(const char *fmt, ...) __attribute__((format(LOG_FORMAT, 1, 2)));
static void logf_(const char *fmt, ...)
{
    time_t t = time(NULL);
    char ts[16];
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(stderr, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#ifndef __EMSCRIPTEN__
static void on_signal(int s) { (void)s; stop = 1; }
#endif

typedef struct {
    cyusb_dev_t *dev;
    cymcp_link_t *link;
    cy_gate_t *gate;
    uint16_t tag;                 /* of the UG_SEND waiting for its result */
    int sending;
    uint32_t xtreme_id;
    unsigned hello_at;            /* last UG_HELLO */
    int app_up;
    /* records from the Xtreme */
    uint8_t rec[4 + UG_MAX];
    int rec_len;
    /* console output, a line at a time */
    char con[512];
    int con_len;
    /* the chat bridge */
    relay_t *relay;
    int chat_want, chat_on;
    uint16_t chat_tag;
    struct { uint32_t from, p0; } seen[64];
    int seen_at;
    unsigned lines_out, lines_in;
} cywig_t;

static unsigned now(cywig_t *w) { return w->dev->now_ms(w->dev); }

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xffff); }
static unsigned get16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

static int send_rec(cywig_t *w, int type, const uint8_t *payload, int len)
{
    uint8_t b[4 + UG_MAX];
    b[0] = (uint8_t)type;
    b[1] = 0;
    put16(b + 2, (unsigned)len);
    memcpy(b + 4, payload, (size_t)len);
    int r = cymcp_write(w->link, 2 * UG_PIPE - 1, b, 4 + len, 10000);
    if (r != 4 + len) logf_("the Xtreme did not take a record (%d)", r);
    return r == 4 + len;
}

/* ---- the gate's side ---- */

static bool gate_msg_send(void *user, uint32_t to, uint16_t msgid, uint32_t p0, uint32_t p1,
                          const char *process, const uint8_t *data, size_t len)
{
    cywig_t *w = user;
    uint8_t b[UG_MAX];
    size_t nl = strlen(process) + 1;
    if (16 + nl + len > sizeof b) {
        logf_("message for %08x too long for the cable (%zu bytes)", (unsigned)to, len);
        return false;
    }
    put16(b, ++w->tag);
    put32(b + 2, to);
    put16(b + 6, msgid);
    put32(b + 8, p0);
    put32(b + 12, p1);
    memcpy(b + 16, process, nl);
    if (len) memcpy(b + 16 + nl, data, len);
    w->sending = 1;
    if (verbose) logf_("-> %08x msg %04x p0 %08x p1 %08x to %s, %zu bytes", (unsigned)to, msgid, (unsigned)p0, (unsigned)p1, process, len);
    return send_rec(w, UG_SEND, b, (int)(16 + nl + len));
}

static void gate_log(void *user, const char *line)
{
    (void)user;
    logf_("gate: %s", line);
}

/* ---- the Xtreme's side ---- */

/* ---- the chat bridge ---- */

static const char *room_name(int room)
{
    static const char *const names[] = { "General", "Romance", "Computing", "Sports", "Games", "Music" };
    return room >= 0 && room < 6 ? names[room] : "a user room";
}

/* A line a Cybiko around said: to the room on the Internet, as that Cybiko. */
static void chat_from_radio(cywig_t *w, uint32_t from, uint16_t msgid, uint32_t p0, uint32_t p1, const uint8_t *data, int len)
{
    char id[8];
    cyr_cyid_str(from, id);
    if (msgid != CYR_MSG_CHAT) {
        logf_("Chat message %04x from @%s (%d bytes), not a line; not passed on", msgid, id, len);
        return;
    }
    for (int i = 0; i < 64; i++)
        if (w->seen[i].from == from && w->seen[i].p0 == p0) return;     /* a copy */
    w->seen[w->seen_at].from = from;
    w->seen[w->seen_at].p0 = p0;
    w->seen_at = (w->seen_at + 1) % 64;
    int tl = 0;
    while (tl < len && tl < CYR_CHAT_MAX && data[tl]) tl++;
    char text[CYR_CHAT_MAX + 1];
    memcpy(text, data, (size_t)tl);
    text[tl] = 0;
    logf_("chat: @%s in %s: %s", id, room_name((int)p1), text);
    if (!w->relay) return;
    if (!relay_online(w->relay)) logf_("chat: not on the Internet now; that line stays here");
    else { relay_say(w->relay, from, (int)p1, p0, text); w->lines_out++; }
}

/* A line from the room on the Internet: to the Cybikos around, from the
 * Xtreme (that is who they hear), with the sender's name in front. */
static void chat_from_relay(void *user, uint32_t from, const char *nick, int room, const char *text)
{
    cywig_t *w = user;
    char id[8], line[CYR_CHAT_MAX + 1];
    cyr_cyid_str(from, id);
    if (*nick) snprintf(line, sizeof line, "%s: %s", nick, text);
    else snprintf(line, sizeof line, "@%s: %s", id, text);
    logf_("chat: Internet, %s: %s", room_name(room), line);
    if (!w->app_up || !w->chat_on) {
        logf_("chat: the Xtreme isn't bridging chat now; that line isn't passed on");
        return;
    }
    uint8_t b[16 + 4 + sizeof line];
    size_t tl = strlen(line) + 1;
    uint32_t clock = now(w) | 1;     /* the line's clock: receivers drop its copies */
    /* three times, like Chat does: a broadcast has no acknowledgement */
    for (int copy = 0; copy < 3; copy++) {
        w->chat_tag = (uint16_t)((w->chat_tag + 1) & ~CHAT_TAG);
        put16(b, (unsigned)(CHAT_TAG | w->chat_tag));
        put32(b + 2, UG_BROADCAST);
        put16(b + 6, CYR_MSG_CHAT);
        put32(b + 8, clock);
        put32(b + 12, (uint32_t)room);
        memcpy(b + 16, "#25", 4);
        memcpy(b + 20, line, tl);
        if (!send_rec(w, UG_SEND, b, (int)(20 + tl))) return;
    }
    w->lines_in++;
}

static void relay_log(void *user, const char *line)
{
    (void)user;
    logf_("%s", line);
}

static void set_chat(cywig_t *w, int on)
{
    w->chat_want = on;
    if (on && !w->relay) logf_("chat: start with --chat to have the Internet side");
    logf_("chat bridge: %s", on ? "on" : "off");
    if (w->app_up) { uint8_t b = (uint8_t)on; send_rec(w, UG_CHAT, &b, 1); }
}

#ifdef __EMSCRIPTEN__
/* The page's switch and stop button. They only leave word: the work is
 * done in the loop (the page calls in while the loop waits for USB). */
static volatile int chat_asked = -1;
EMSCRIPTEN_KEEPALIVE void cywig_chat(int on) { chat_asked = on; }
EMSCRIPTEN_KEEPALIVE void cywig_stop(void) { stop = 1; }
static void read_stdin(cywig_t *w)
{
    if (chat_asked < 0) return;
    set_chat(w, chat_asked);
    chat_asked = -1;
}
#elif defined _WIN32
/* "chat on" / "chat off" typed while it runs: Windows can't poll a console,
 * so a thread reads the lines and the loop takes what it left. */
#include <pthread.h>
static volatile int chat_asked = -1, bad_line;
static void *stdin_thread(void *arg)
{
    char b[64];
    (void)arg;
    while (fgets(b, sizeof b, stdin)) {
        b[strcspn(b, "\r\n")] = 0;
        if (!strcmp(b, "chat on") || !strcmp(b, "chat off")) chat_asked = !strcmp(b, "chat on");
        else if (*b) bad_line = 1;
    }
    return NULL;
}
static void read_stdin(cywig_t *w)
{
    static int started;
    if (!started) {
        pthread_t t;
        started = 1;
        if (!pthread_create(&t, NULL, stdin_thread, NULL)) pthread_detach(t);
    }
    if (bad_line) { bad_line = 0; logf_("type \"chat on\" or \"chat off\""); }
    if (chat_asked < 0) return;
    set_chat(w, chat_asked);
    chat_asked = -1;
}
#else
/* "chat on" / "chat off" typed while it runs */
static void read_stdin(cywig_t *w)
{
    static char b[64];
    static int bl;
    struct pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    int r = (int)read(0, b + bl, sizeof b - 1 - (size_t)bl);
    if (r <= 0) return;
    bl += r;
    b[bl] = 0;
    char *nl;
    while ((nl = strchr(b, '\n'))) {
        *nl = 0;
        if (!strcmp(b, "chat on") || !strcmp(b, "chat off")) {
            set_chat(w, !strcmp(b, "chat on"));
        } else if (*b) {
            logf_("type \"chat on\" or \"chat off\"");
        }
        bl -= (int)(nl + 1 - b);
        memmove(b, nl + 1, (size_t)bl + 1);
    }
    if (bl == (int)sizeof b - 1) bl = 0;
}
#endif

static void record(cywig_t *w, const uint8_t *r, int len)
{
    const uint8_t *p = r + 4;
    int n = len - 4;
    switch (r[0]) {
    case UG_HELLO:
        if (n < 6) break;
        if (!w->app_up || w->xtreme_id != get32(p + 2))
            logf_("USB Gate running on Cybiko %08x (version %d, gate mode %s)", (unsigned)get32(p + 2), p[0],
                  p[1] & UG_STATE_GATE ? "on" : "OFF");
        if (w->app_up && !!(p[1] & UG_STATE_CHAT) != w->chat_on)
            logf_("chat bridge %s on the Xtreme", p[1] & UG_STATE_CHAT ? "on" : "off");
        w->xtreme_id = get32(p + 2);
        w->app_up = 1;
        w->hello_at = now(w);
        w->chat_on = !!(p[1] & UG_STATE_CHAT);
        if (w->chat_on != w->chat_want) {
            uint8_t on = (uint8_t)w->chat_want;
            send_rec(w, UG_CHAT, &on, 1);
        }
        return;
    case UG_MSG:
        if (n < 16) break;
        if (verbose)
            logf_("<- %08x msg %04x p0 %08x p1 %08x port %x, %d bytes", (unsigned)get32(p), get16(p + 4),
                  (unsigned)get32(p + 6), (unsigned)get32(p + 10), get16(p + 14), n - 16);
        if (get16(p + 14) == UG_CHAT_PORT) chat_from_radio(w, get32(p), (uint16_t)get16(p + 4), get32(p + 6), get32(p + 10),
                                                           p + 16, n - 16);
        else cy_gate_msg_in(w->gate, get32(p), (uint16_t)get16(p + 4), get32(p + 6), get32(p + 10), p + 16, (size_t)(n - 16));
        return;
    case UG_RESULT: {
        if (n < 3) break;
        int code = p[2];
        if (get16(p) & CHAT_TAG) {
            if (code != UG_DL_SUCCESS || verbose)
                logf_("chat send %u to the Cybikos around: %s", get16(p) & ~CHAT_TAG,
                      code == UG_DL_SUCCESS ? "sent" : code == UG_DL_ABORT ? "nobody took it" :
                      code == UG_DL_TIMEOUT ? "timed out" : "failed");
            return;
        }
        if (!w->sending || get16(p) != w->tag) {
            logf_("delivery result %d for send %u, not the one waiting (%u)", code, get16(p), w->tag);
            return;
        }
        w->sending = 0;
        if (code != UG_DL_SUCCESS)
            logf_("delivery failed: %s", code == UG_DL_ABORT ? "nobody took it" : code == UG_DL_TIMEOUT ? "timed out" : "error");
        cy_gate_msg_done(w->gate, code == UG_DL_SUCCESS ? CY_GATE_DELIVERED : code == UG_DL_ABORT ? CY_GATE_NOBODY : CY_GATE_FAILED);
        return;
    }
    case UG_LOG:
        logf_("xtreme: %.*s", n, (const char *)p);
        return;
    default:
        logf_("record of type %02x from the Xtreme, not one we know (%d bytes)", r[0], n);
        return;
    }
    logf_("short record of type %02x from the Xtreme (%d bytes)", r[0], n);
}

static void read_records(cywig_t *w)
{
    for (;;) {
        int want = w->rec_len < 4 ? 4 - w->rec_len : 4 + (int)get16(w->rec + 2) - w->rec_len;
        if (w->rec_len >= 4 && get16(w->rec + 2) > UG_MAX) {
            logf_("record of %u bytes from the Xtreme: out of step, starting again", get16(w->rec + 2));
            w->rec_len = 0;
            continue;
        }
        if (want == 0) {
            record(w, w->rec, w->rec_len);
            w->rec_len = 0;
            continue;
        }
        int r = cymcp_read(w->link, 2 * UG_PIPE, w->rec + w->rec_len, want, 0);
        if (r <= 0) return;
        w->rec_len += r;
    }
}

static void read_console(cywig_t *w)
{
    char b[256];
    int r;
    while ((r = cymcp_read(w->link, CYMCP_CONSOLE_OUT, b, sizeof b, 0)) > 0) {
        for (int i = 0; i < r; i++) {
            char c = b[i];
            if (c == '\n' || c == '\r' || c == 0 || w->con_len == (int)sizeof w->con - 1) {
                w->con[w->con_len] = 0;
                if (w->con_len && strcmp(w->con, " ")) logf_("console: %s", w->con);
                w->con_len = 0;
            } else {
                w->con[w->con_len++] = c;
            }
        }
    }
}

static int console(cywig_t *w, const char *line)
{
    logf_("console <- %s", line);
    return cymcp_write(w->link, CYMCP_CONSOLE_IN, line, (int)strlen(line) + 1, 5000) > 0;
}

/* The last sign of life from USB Gate: a HELLO, or any frame on its pipes
 * (a busy Xtreme sends HELLOs late, but its pipes keep talking; starting
 * a second USB Gate next to a slow one upsets CyOS). */
static unsigned app_heard(cywig_t *w)
{
    unsigned h = w->hello_at;
    unsigned a = cymcp_heard(w->link, 2 * UG_PIPE), b = cymcp_heard(w->link, 2 * UG_PIPE - 1);
    if (a && (int)(a - h) > 0) h = a;
    if (b && (int)(b - h) > 0) h = b;
    return h;
}

static int run(cywig_t *w)
{
    unsigned next_ping = 0, started_at = 0;
    int starts = 0;
    cymcp_listen(w->link, CYMCP_CONSOLE_OUT, 1);
    cymcp_listen(w->link, 2 * UG_PIPE, 1);
    while (!stop) {
        unsigned t = now(w);
        if (cymcp_poll(w->link, 20) < 0) { logf_("the Xtreme went away"); return 1; }
        read_console(w);
        read_records(w);
        if (w->relay) relay_poll(w->relay);
        read_stdin(w);
        uint32_t wait = cy_gate_poll(w->gate, t);
        (void)wait;
        if (w->app_up && (int)(t - app_heard(w)) > APP_GONE_MS) {   /* signed: a HELLO can be newer than t */
            logf_("USB Gate stopped answering (nothing from it for %d s)", (int)(t - app_heard(w)) / 1000);
            w->app_up = 0;
            started_at = 0;
            starts = 0;
        }
        if (w->app_up && (int)(t - next_ping) >= 0) {
            next_ping = t + PING_MS;
            send_rec(w, UG_PING, NULL, 0);
        }
        if (!w->app_up && (!started_at || (int)(t - started_at) > 6000)) {
            if (starts++ >= START_TRIES) {
                logf_("USB Gate does not start: is %s on the Xtreme?", APP_NAME);
                starts = 0;
                started_at = t + 30000;
                continue;
            }
            console(w, APP_NAME);
            started_at = t;
        }
    }
    if (w->app_up) send_rec(w, UG_QUIT, NULL, 0);
    return 0;
}

static void usage(const char *me)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --emulator HOST:PORT  use an emulated Xtreme (Dragonfruit's cybiko_host --usb-server PORT)\n"
            "  --home URL            the page for the WAP browser's Home\n"
            "  --flash FILE          a CyOS flash image, for its fonts (pages for Browser.app\n"
            "                        are laid out with them; without it, close stand-ins)\n"
            "  --chat                bridge Chat to the Internet (type \"chat off\" / \"chat on\" to switch)\n"
            "  --room NAME           the relay's room (default public, the web emulator's)\n"
            "  --relay URL           the relay (default " RELAY_URL ")\n"
            "  --nick NAME           the bridge's nickname in the room (default USBGate)\n"
            "  -v                    log every message\n", me);
}

int main(int argc, char **argv)
{
    const char *emulator = NULL, *home = NULL, *flash = NULL;
    const char *relay_url = RELAY_URL, *room = "public", *nick = "USBGate";
    int chat = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--emulator") && v) { emulator = v; i++; }
        else if (!strcmp(a, "--home") && v) { home = v; i++; }
        else if (!strcmp(a, "--flash") && v) { flash = v; i++; }
        else if (!strcmp(a, "--chat")) chat = 1;
        else if (!strcmp(a, "--room") && v) { room = v; i++; }
        else if (!strcmp(a, "--relay") && v) { relay_url = v; i++; }
        else if (!strcmp(a, "--nick") && v) { nick = v; i++; }
        else if (!strcmp(a, "-v")) verbose = 1;
        else { usage(argv[0]); return 2; }
    }
    static cywig_t w;               /* static: a page keeps calling into it */
    memset(&w, 0, sizeof w);
    char err[256] = "";
#ifdef __EMSCRIPTEN__
    (void)emulator;
    w.dev = cyusb_web_open();
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    if (emulator) w.dev = cyusb_tcp_open(emulator, err, sizeof err);
#endif
#ifdef __EMSCRIPTEN__
#elif defined CYWIG_LIBUSB
    else w.dev = cyusb_libusb_open(err, sizeof err);
#else
    else snprintf(err, sizeof err, "built without libusb: use --emulator");
#endif
    if (!w.dev) { logf_("%s", err); return 1; }
    logf_("Xtreme on the cable%s%s", emulator ? " (emulated at " : "", emulator ? emulator : "");
    if (emulator) fprintf(stderr, ")\n");
    w.link = cymcp_open(w.dev);

    cyb_net_t net;
#ifdef __EMSCRIPTEN__
    net_web_init(&net);
#elif defined CYWIG_LIBCURL
    cyb_net_libcurl(&net, NULL);
#else
    cyb_net_curl(&net, NULL);
#endif
    cy_gate_host_t host = { 0 };
    host.user = &w;
    host.log = gate_log;
    host.msg_send = gate_msg_send;
    w.gate = cy_gate_create(0, &host, &net);
    if (home) cy_gate_set_home(w.gate, home);
    if (flash) {
        FILE *f = fopen(flash, "rb");
        if (!f) { logf_("can't open %s: %s", flash, strerror(errno)); return 1; }
        static uint8_t img[4 << 20];
        size_t n = fread(img, 1, sizeof img, f);
        fclose(f);
        cy_gate_set_fonts(w.gate, img, n);
    }

    if (chat) {
        relay_host_t rh = { &w, chat_from_relay, relay_log };
        /* the bridge's own seat in the room: an ID of its own (the 4c
         * prefix every Cybiko ID has, then random bits) */
        uint32_t id;
        if (getentropy(&id, sizeof id)) id = (uint32_t)time(NULL) * 2654435761u;
        id = 0x4c000000u | (id & 0xffffff);
        w.relay = relay_create(relay_url, room, RELAY_ORIGIN, id, nick, &rh);
        w.chat_want = 1;
        logf_("chat bridge on: room %s of %s", room, relay_url);
    }

    int r = run(&w);
    if (w.relay) {
        logf_("chat: %u lines to the Internet, %u from it", w.lines_out, w.lines_in);
        relay_destroy(w.relay);
    }
    const cymcp_stats_t *k = cymcp_stats(w.link);
    logf_("cable: %u frames in, %u out, %u resent, %u bad, %u unexpected", (unsigned)k->frames_in,
          (unsigned)k->frames_out, (unsigned)k->resent, (unsigned)k->bad, (unsigned)k->unexpected);
    cy_gate_destroy(w.gate);
    cymcp_close(w.link);
#ifdef __EMSCRIPTEN__
    EM_ASM({ if (Module.done) Module.done($0); }, r);
#else
    if (emulator) cyusb_tcp_close(w.dev);
#ifdef CYWIG_LIBUSB
    else cyusb_libusb_close(w.dev);
#endif
#endif
    return r;
}
