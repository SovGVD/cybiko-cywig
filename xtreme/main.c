/* USB Gate: makes a stock Cybiko Xtreme the Cybiko Internet Gate (CyWIG)
 * for the Cybikos around it, with a PC on its USB cable doing the web.
 *
 * It turns on CyOS's own gate mode (export 1152: everything the Xtreme
 * sends then carries the gate flag, so the others pick it as their gate),
 * takes the messages that come to the gate's port 93 (a Process named by a
 * port number gets that port's messages) and passes them to the PC over a
 * pipe of CyOS's USB link; the PC sends back the messages to deliver (the
 * pages for the WAP browser and Browser). The records are in proto.h. No
 * change to CyOS or its flash: this is an ordinary application.
 *
 * Two threads: this application's own (screen, keys, the port's messages,
 * sending to the PC) and a reader that waits on the pipe for the PC. */
#include "../cybiko-browser/cyapp/cyos.h"
#include "proto.h"

#define MSG_AUTODELETE 0x80000000UL
#define DELIVER_MS 30000L
#define PENDING 8
#define HELLO_MS 3000
#define PC_GONE_MS 10000L

extern void *cy_mcp_pipe_factory;
void *cy_PipeFactory_open(void *factory, const char *name, long n, long zero);
void cy_PipeFactory_release(void *factory, void *pipe, long zero);
long cy_MCPInput_read(void *in, void *buf, long len);
u8 cy_MCPOutput_flush(void *out);
long cy_MCPOutput_write(void *out, const void *buf, long len);
void cy_set_gate_mode(u8 on);
u8 cy_get_gate_mode(void);
struct Message *cy_Queue_peek_message(struct Queue *q, int remove, int min, int max);

/* an IPipe's methods (CyOS's console and loader call them this way) */
#define PIPE_VT(p) (*(void ***)((char *)(p) + 8))
#define STREAM_TIMEOUT(s) (*(long *)((char *)*(void **)(s) + 4))

static u32 app_mem[CWINAPP_SIZE / 4 + 4];
static void *app_vt[20];
static struct Process *me;
static u32 port_mem[PROCESS_SIZE / 4];
static struct Process *port;          /* only its queue: port 93's messages */
static u32 rd_mem[PROCESS_SIZE / 4];
static void *rd_vt[20];
static struct Process *rd;

static void *pipe_, *in, *out;
static volatile u8 quitting, rd_done;

/* sends waiting for their delivery: the reader fills a slot (state 1 ->
 * 2), the application thread reports and frees it (2 -> 0) */
static struct {
    volatile u8 state;
    u16 tag;
    struct Message *m;
    struct Buffer buf;
} pend[PENDING];

/* what the screen shows */
static long requests, answers, failed;
static volatile long pc_heard;        /* time of the last record from the PC */
static long now_ms;
static u8 dirty = 1;
static struct DisplayGraphics *gfx;

static u8 tx[4 + 20 + UG_MAX];
static u8 rx[UG_MAX + 4];

static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { put16(p, (u16)(v >> 16)); put16(p + 2, (u16)v); }
static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 get32(const u8 *p) { return (u32)get16(p) << 16 | get16(p + 2); }

/* One record to the PC. Only the application thread writes. */
static void send_rec(int type, int len)
{
    tx[0] = (u8)type;
    tx[1] = 0;
    put16(tx + 2, (u16)len);
    if (!out) return;
    cy_MCPOutput_write(out, tx, 4 + len);
    cy_MCPOutput_flush(out);
}

static void hello(void)
{
    const char *name = "USB Gate";
    int n = 0;
    tx[4] = UG_VERSION;
    tx[5] = cy_get_gate_mode();
    put32(tx + 6, (u32)cy_get_own_id());
    while (name[n]) { tx[10 + n] = (u8)name[n]; n++; }
    send_rec(UG_HELLO, 6 + n);
}

static void log_line(const char *s)
{
    int n = 0;
    while (s[n] && n < 120) { tx[4 + n] = (u8)s[n]; n++; }
    send_rec(UG_LOG, n);
}

/* A message that came to the gate's port: over to the PC. */
static void forward(struct Message *m)
{
    u8 *p = tx + 4;
    long size = 0;
    if (cy_Message_has_buffer(m)) {
        struct Buffer *b = cy_Message_get_buffer(m);
        size = (long)cy_Buffer_get_size(b);
        if (size > UG_MAX - 16) {
            char s[48];
            cy_sprintf(s, "message %x cut from %ld bytes", m->msgid, size);
            log_line(s);
            size = UG_MAX - 16;
        }
        if (size > 0) cy_Buffer_load(b, p + 16, 0, (u32)size);
    }
    put32(p, (u32)m->cyid_from);
    put16(p + 4, (u16)m->msgid);
    put32(p + 6, (u32)m->param[0]);
    put32(p + 10, (u32)m->param[1]);
    put16(p + 14, (u16)(u32)m->dst_name);
    send_rec(UG_MSG, 16 + (int)size);
    requests++;
    dirty = 1;
}

static void results(void)
{
    int i;
    for (i = 0; i < PENDING; i++) {
        int st;
        if (pend[i].state != 2) continue;
        st = cy_Message_check_delivery(pend[i].m);
        if (st < UG_DL_ABORT && st != UG_DL_NONE) continue;
        put16(tx + 4, pend[i].tag);
        tx[6] = (u8)st;
        tx[7] = 0;
        send_rec(UG_RESULT, 4);
        if (st == UG_DL_SUCCESS) answers++;
        else failed++;
        dirty = 1;
        cy_Message_delete(pend[i].m);
        pend[i].state = 0;
    }
}

/* ---- the reader thread ---- */

static void pause_ms(long ms)
{
    struct Message *m = cy_Queue_get_message(PROCESS_QUEUE(rd), ms, 0, 0x7fff);
    if (m) cy_Message_delete(m);
}

/* n bytes from the PC, or 0 when stopping */
static u8 read_all(u8 *p, int n)
{
    while (n > 0) {
        long r;
        if (quitting) return 0;
        r = cy_MCPInput_read(in, p, n);
        if (r < 0) { pause_ms(500); continue; }   /* no PC on the cable */
        p += r;
        n -= (int)r;
    }
    return 1;
}

static void do_send(int len)
{
    struct Message *m;
    const char *name = (const char *)rx + 16;
    int nl = 0, i, slot = -1;
    long dl;
    if (len < 17) return;
    while (16 + nl < len && name[nl]) nl++;
    if (16 + nl >= len) return;               /* no NUL: not ours */
    while (slot < 0 && !quitting) {
        for (i = 0; i < PENDING; i++) if (!pend[i].state) { slot = i; break; }
        if (slot < 0) pause_ms(50);
    }
    if (slot < 0) return;
    pend[slot].state = 1;
    pend[slot].tag = get16(rx);
    m = cy_Message_new(sizeof(struct Message));
    if (!m) { pend[slot].state = 0; return; }
    m->msgid = (short)get16(rx + 6);
    m->param[0] = (long)get32(rx + 8);
    m->param[1] = (long)get32(rx + 12);
    dl = len - (16 + nl + 1);
    if (dl > 0) {
        cy_Buffer_ctor(&pend[slot].buf, (u32)dl, 16);
        cy_Buffer_store(&pend[slot].buf, rx + 16 + nl + 1, 0, (u32)dl);
        cy_Message_attach_buffer(m, &pend[slot].buf);
    }
    cy_Message_deliver(m, name, (cyid_t)get32(rx + 2), DELIVER_MS);
    pend[slot].m = m;
    pend[slot].state = 2;
}

static void reader(struct Process *p)
{
    u8 h[4];
    (void)p;
    while (!quitting && read_all(h, 4)) {
        int len = get16(h + 2);
        if (len > UG_MAX) len = UG_MAX;     /* the PC never sends that much */
        if (!read_all(rx, len)) break;
        pc_heard = now_ms ? now_ms : 1;
        if (h[0] == UG_SEND) do_send(len);
        else if (h[0] == UG_QUIT) quitting = 1;
        /* UG_PING: the application thread says hello on its own */
    }
    rd_done = 1;
}

/* ---- screen ---- */

static void line(int y, const char *s)
{
    cy_Graphics_draw_text(gfx, s, 4, y);
}

static void paint(void)
{
    char s[40];
    u8 pc = pc_heard && now_ms - pc_heard < PC_GONE_MS;
    if (!cy_Process_has_focus(me)) return;
    cy_TGraph_set_color(gfx, CLR_WHITE);
    cy_TGraph_fill_screen(gfx, CLR_WHITE);
    cy_TGraph_set_color(gfx, CLR_BLACK);
    cy_Graphics_set_font(gfx, cy_cool_bold_font);
    line(2, "USB Gate");
    cy_Graphics_set_font(gfx, cy_cool_normal_font);
    line(20, pc ? "PC: connected" : "PC: waiting for it");
    line(34, cy_get_gate_mode() ? "Gate for Cybikos around" : "Gate mode off");
    cy_sprintf(s, "Requests %ld", requests);
    line(48, s);
    cy_sprintf(s, "Answers %ld, failed %ld", answers, failed);
    line(62, s);
    cy_Graphics_set_font(gfx, cy_mini_normal_font);
    line(86, "Esc: stop the gate");
    cy_DisplayGraphics_show(gfx);
    dirty = 0;
}

/* ---- the application ---- */

static void run(struct Process *p)
{
    long next_hello = 0;
    u8 was_pc = 0;
    int i;
    gfx = cy_Process_get_display();
    cy_Process_request_focus(p);
    pipe_ = cy_mcp_pipe_factory ? cy_PipeFactory_open(cy_mcp_pipe_factory, "USBGate", UG_PIPE, 0) : 0;
    if (pipe_) {
        in = ((void *(*)(void *))PIPE_VT(pipe_)[4])(pipe_);
        out = ((void *(*)(void *))PIPE_VT(pipe_)[5])(pipe_);
        STREAM_TIMEOUT(in) = 2000;
        STREAM_TIMEOUT(out) = 3000;
    }
    port = cy_Process_ctor(port_mem, (const char *)UG_PORT, 512);
    cy_set_gate_mode(1);
    if (in) {
        rd = cy_Process_ctor(rd_mem, "UGateRd", 1500);
        for (i = 0; i < 20; i++) rd_vt[i] = PROCESS_VTABLE(rd)[i];
        rd_vt[4] = (void *)reader;
        PROCESS_VTABLE(rd) = rd_vt;
        cy_Thread_start(rd, 0);
    }
    paint();
    while (!quitting) {
        struct Message *m = cy_Queue_get_message(PROCESS_QUEUE(p), 50, 0, 0x7fff);
        now_ms += 50;                   /* close enough for the screen */
        if (m) {
            switch (m->msgid) {
            case MSG_GOTFOCUS: {
                void *w = CWINAPP_WINDOW(p);
                u8 (*proc)(void *, struct Message *) = (*(void ***)((char *)w + 0x10))[6];
                proc(w, m);
                dirty = 1;
                break;
            }
            case MSG_KEYDOWN:
                if (((struct KeyParam *)m->param)->scancode == KEY_ESC) quitting = 1;
                break;
            case MSG_QUIT:
            case MSG_SHUTUP:
                quitting = 1;
                break;
            }
            cy_Message_delete(m);
        }
        while ((m = cy_Queue_peek_message(PROCESS_QUEUE(port), 1, 0, 0x7fff)) != 0) {
            forward(m);
            cy_Message_delete(m);
        }
        results();
        if (now_ms >= next_hello) {
            next_hello = now_ms + HELLO_MS;
            if (out) hello();
        }
        if ((pc_heard && now_ms - pc_heard < PC_GONE_MS) != was_pc) { was_pc = !was_pc; dirty = 1; }
        if (dirty) paint();
    }
    cy_set_gate_mode(0);
    /* the reader stops at its next read (at most 2 s) */
    for (i = 0; i < 60 && rd && !rd_done; i++) {
        struct Message *m = cy_Queue_get_message(PROCESS_QUEUE(p), 50, 0, 0x7fff);
        if (m) cy_Message_delete(m);
    }
    for (i = 0; i < PENDING; i++)
        if (pend[i].state == 2) cy_Message_delete(pend[i].m);
    cy_Process_dtor(port, LEAVE_MEMORY);
    if (pipe_) cy_PipeFactory_release(cy_mcp_pipe_factory, pipe_, 0);
    *(u16 *)((char *)p + 0x4c) |= 4;    /* have the desktop drawn again */
}

long main(int argc, char **argv, char start)
{
    void **kvt;
    int i;
    (void)argc;
    (void)argv;
    if (!start) return 0;
    me = cy_cWinApp_ctor(app_mem, "USB Gate", 3000);
    kvt = PROCESS_VTABLE(me);
    for (i = 0; i < 20; i++) app_vt[i] = kvt[i];
    app_vt[7] = (void *)run;            /* a cWinApp's thread runs slot 7 */
    PROCESS_VTABLE(me) = app_vt;
    cy_Thread_start(me, 0);
    return 0;
}
