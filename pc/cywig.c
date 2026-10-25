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
 * starts the application through CyOS's console. */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cymcp.h"
#include "cyusb_tcp.h"
#ifdef CYWIG_LIBUSB
#include "cyusb_libusb.h"
#endif
#include "cy_gate.h"
#include "cyb_net_curl.h"
#include "../xtreme/proto.h"

#define PING_MS 3000
#define APP_GONE_MS 8000
#define START_TRIES 3
#define APP_NAME "USBGate.app"

static volatile sig_atomic_t stop;
static int verbose;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
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

static void on_signal(int s) { (void)s; stop = 1; }

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

static void record(cywig_t *w, const uint8_t *r, int len)
{
    const uint8_t *p = r + 4;
    int n = len - 4;
    switch (r[0]) {
    case UG_HELLO:
        if (n < 6) break;
        if (!w->app_up || w->xtreme_id != get32(p + 2))
            logf_("USB Gate running on Cybiko %08x (version %d, gate mode %s)", (unsigned)get32(p + 2), p[0],
                  p[1] ? "on" : "OFF");
        w->xtreme_id = get32(p + 2);
        w->app_up = 1;
        w->hello_at = now(w);
        return;
    case UG_MSG:
        if (n < 16) break;
        if (verbose)
            logf_("<- %08x msg %04x p0 %08x p1 %08x port %x, %d bytes", (unsigned)get32(p), get16(p + 4),
                  (unsigned)get32(p + 6), (unsigned)get32(p + 10), get16(p + 14), n - 16);
        cy_gate_msg_in(w->gate, get32(p), (uint16_t)get16(p + 4), get32(p + 6), get32(p + 10), p + 16, (size_t)(n - 16));
        return;
    case UG_RESULT: {
        if (n < 3) break;
        int code = p[2];
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
        uint32_t wait = cy_gate_poll(w->gate, t);
        (void)wait;
        if (w->app_up && t - w->hello_at > APP_GONE_MS) {
            logf_("USB Gate stopped answering");
            w->app_up = 0;
            started_at = 0;
            starts = 0;
        }
        if ((int)(t - next_ping) >= 0) {
            next_ping = t + PING_MS;
            send_rec(w, UG_PING, NULL, 0);
        }
        if (!w->app_up && (!started_at || t - started_at > 6000)) {
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
            "  -v                    log every message\n", me);
}

int main(int argc, char **argv)
{
    const char *emulator = NULL, *home = NULL, *flash = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--emulator") && v) { emulator = v; i++; }
        else if (!strcmp(a, "--home") && v) { home = v; i++; }
        else if (!strcmp(a, "--flash") && v) { flash = v; i++; }
        else if (!strcmp(a, "-v")) verbose = 1;
        else { usage(argv[0]); return 2; }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    cywig_t w;
    memset(&w, 0, sizeof w);
    char err[256] = "";
    if (emulator) w.dev = cyusb_tcp_open(emulator, err, sizeof err);
#ifdef CYWIG_LIBUSB
    else w.dev = cyusb_libusb_open(err, sizeof err);
#else
    else snprintf(err, sizeof err, "built without libusb: use --emulator");
#endif
    if (!w.dev) { logf_("%s", err); return 1; }
    logf_("Xtreme on the cable%s%s", emulator ? " (emulated at " : "", emulator ? emulator : "");
    if (emulator) fprintf(stderr, ")\n");
    w.link = cymcp_open(w.dev);

    cyb_net_t net;
    cyb_net_curl(&net, NULL);
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

    int r = run(&w);
    const cymcp_stats_t *k = cymcp_stats(w.link);
    logf_("cable: %u frames in, %u out, %u resent, %u bad, %u unexpected", (unsigned)k->frames_in,
          (unsigned)k->frames_out, (unsigned)k->resent, (unsigned)k->bad, (unsigned)k->unexpected);
    cy_gate_destroy(w.gate);
    cymcp_close(w.link);
    if (emulator) cyusb_tcp_close(w.dev);
#ifdef CYWIG_LIBUSB
    else cyusb_libusb_close(w.dev);
#endif
    return r;
}
