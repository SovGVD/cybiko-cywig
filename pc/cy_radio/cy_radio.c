/* Copied from Dragonfruit (https://github.com/SovGVD/Dragonfruit-JC4827W543-esp32,
 * src/components/cy_radio); change it there first. */
/* The Cybiko radio protocol (see cy_radio.h and
 * radio-protocol.md in cybiko-docs, https://github.com/SovGVD/cybiko-docs). */
#include "cy_radio.h"
#include <string.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

/* ---- Reed-Solomon over GF(256), x^8+x^4+x^3+x^2+1 (the boot ROM's tables) ----
 * As a codeword, parity byte j is the coefficient of x^j and data byte i
 * that of x^(n+i); the generator's roots are a^1..a^n (a = 2). */
static uint8_t gf_exp[512], gf_log[256];
static uint8_t gen20[21], gen80[81];

static uint8_t gf_mul(uint8_t a, uint8_t b) { return a && b ? gf_exp[gf_log[a] + gf_log[b]] : 0; }

static void rs_gen(uint8_t *g, int n)
{
    memset(g, 0, (size_t)n + 1);
    g[0] = 1;
    for (int j = 1; j <= n; j++) {
        const uint8_t r = gf_exp[j];
        for (int i = j; i > 0; i--) g[i] = (uint8_t)(g[i - 1] ^ gf_mul(g[i], r));
        g[0] = gf_mul(g[0], r);
    }
}

static void rs_init(void)
{
    if (gf_exp[0]) return;
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = gf_exp[i + 255] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    rs_gen(gen20, 20);
    rs_gen(gen80, 80);
}

static void rs_parity(uint8_t *pkt, size_t len)
{
    const int k = len == CYR_SHORT ? CYR_HEAD + CYR_SHORT_BODY : CYR_HEAD + CYR_LONG_BODY, n = (int)len - k;
    const uint8_t *g = len == CYR_SHORT ? gen20 : gen80;
    uint8_t r[80] = { 0 };
    for (int d = k - 1; d >= 0; d--) {      /* highest coefficient first */
        const uint8_t fb = pkt[d] ^ r[n - 1];
        memmove(r + 1, r, (size_t)n - 1);
        r[0] = 0;
        if (fb)
            for (int i = 0; i < n; i++) r[i] ^= gf_mul(fb, g[i]);
    }
    memcpy(pkt + k, r, (size_t)n);
}

/* CRC-16/MODBUS */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xffff;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c & 1 ? (uint16_t)((c >> 1) ^ 0xa001) : (uint16_t)(c >> 1);
    }
    return c;
}

/* ---- packets ---- */

void cyr_header(uint8_t *pkt, uint32_t to, uint32_t from, uint8_t channel,
                uint8_t type_flags, uint8_t part, uint8_t number)
{
    put32(pkt, to);
    put32(pkt + 4, from);
    pkt[8] = (uint8_t)(0xc0 | (channel & 0x3f));
    pkt[9] = type_flags;
    pkt[10] = part;
    pkt[11] = number;
}

void cyr_seal(uint8_t *pkt, size_t len)
{
    rs_init();
    put16(pkt + 12, crc16(pkt + CYR_HEAD, cyr_body_len(len)));
    put16(pkt + 14, crc16(pkt, 14));
    rs_parity(pkt, len);
}

bool cyr_check(const uint8_t *pkt, size_t len)
{
    if (len != CYR_SHORT && len != CYR_LONG) return false;
    if ((pkt[8] & 0xc0) != 0xc0) return false;
    return get16(pkt + 14) == crc16(pkt, 14) && get16(pkt + 12) == crc16(pkt + CYR_HEAD, cyr_body_len(len));
}

size_t cyr_ping(uint8_t *pkt, uint32_t from, uint8_t channel, uint8_t version,
                const char *nick, int age, bool female)
{
    memset(pkt, 0, CYR_SHORT);
    cyr_header(pkt, CYR_BROADCAST, from, channel, CYR_T_PING | (nick ? CYR_F_PING_DATA : 0), 0, 0);
    if (nick) {
        uint8_t *d = pkt + CYR_HEAD;
        d[0] = version ? version : 1;
        strncpy((char *)d + 4, nick, 8);
        d[12] = (uint8_t)((female ? 0x80 : 0) | (age >= 0 && age < 0x7f ? age : 0x7f));
    }
    cyr_seal(pkt, CYR_SHORT);
    return CYR_SHORT;
}

bool cyr_ping_details(const uint8_t *pkt, char *nick, int *age, bool *female)
{
    if (cyr_type(pkt) != CYR_T_PING || !(pkt[9] & CYR_F_PING_DATA)) return false;
    const uint8_t *d = pkt + CYR_HEAD;
    memcpy(nick, d + 4, 8);
    nick[8] = 0;
    if (age) *age = (d[12] & 0x7f) == 0x7f ? -1 : d[12] & 0x7f;
    if (female) *female = (d[12] & 0x80) != 0;
    return true;
}

size_t cyr_ack(uint8_t *pkt, const uint8_t *in, uint32_t from, uint8_t channel, int code)
{
    memset(pkt, 0, CYR_SHORT);
    cyr_header(pkt, cyr_from(in), from, channel, CYR_T_ACK, in[10], in[11]);
    pkt[CYR_HEAD] = (uint8_t)(code << 4);
    cyr_seal(pkt, CYR_SHORT);
    return CYR_SHORT;
}

size_t cyr_query(uint8_t *pkt, const uint8_t *sent, uint8_t channel)
{
    memset(pkt, 0, CYR_SHORT);
    cyr_header(pkt, cyr_to(sent), cyr_from(sent), channel, CYR_T_QUERY, sent[10], sent[11]);
    cyr_seal(pkt, CYR_SHORT);
    return CYR_SHORT;
}

/* ---- messages ----
 * Body: flags, extra, msgid, [p0], [p1], target (port, or a NUL-ended
 * process name with flag 20), [length], data. A message too big for one
 * packet goes in parts: part 1 has flag 02 and no length byte (its data
 * runs to the end), middle parts start 02 00 and the last 03 00 length. */
#define M_NAME 0x20
#define M_DATA 0x10
#define M_P0 0x08
#define M_P1 0x04
#define M_MORE 0x02
#define M_LAST 0x01

int cyr_msg_packets(const cyr_msg_t *m, uint32_t to, uint32_t from, uint8_t channel,
                    bool ack_me, uint8_t number, uint8_t out[][CYR_LONG], int max)
{
    uint8_t head[CYR_LONG_BODY];
    size_t n = 4;
    head[0] = 0;
    head[1] = m->extra;
    put16(head + 2, m->id);
    if (m->p0) { head[0] |= M_P0; put32(head + n, m->p0); n += 4; }
    if (m->p1) { head[0] |= M_P1; put32(head + n, m->p1); n += 4; }
    if (m->name) {
        size_t l = strlen(m->name) + 1;
        if (n + l > sizeof head) return 0;
        head[0] |= M_NAME;
        memcpy(head + n, m->name, l);
        n += l;
    } else {
        put16(head + n, m->port);
        n += 2;
    }
    const size_t len = m->data ? m->len : 0;
    if (m->data) head[0] |= M_DATA;
    const uint8_t type = (uint8_t)(CYR_T_MSG | (ack_me ? CYR_F_ACK_ME : 0));
    if (max < 1) return 0;
    if (n + (m->data ? 1 : 0) + len <= CYR_LONG_BODY) {
        uint8_t *p = out[0];
        memset(p, 0, CYR_LONG);
        cyr_header(p, to, from, channel, type, 0, number);
        memcpy(p + CYR_HEAD, head, n);
        if (m->data) {
            p[CYR_HEAD + n] = (uint8_t)len;
            memcpy(p + CYR_HEAD + n + 1, m->data, len);
        }
        cyr_seal(p, CYR_LONG);
        return 1;
    }
    /* in parts */
    size_t first = CYR_LONG_BODY - n, rest = len - first;
    int parts = 2;
    while (rest > CYR_LONG_BODY - 3) { rest -= CYR_LONG_BODY - 2; parts++; }
    if (parts > max || parts > 255) return 0;
    const uint8_t *d = m->data;
    for (int i = 1; i <= parts; i++) {
        uint8_t *p = out[i - 1], *b = p + CYR_HEAD;
        memset(p, 0, CYR_LONG);
        cyr_header(p, to, from, channel, type, (uint8_t)i, number);
        if (i == 1) {
            memcpy(b, head, n);
            b[0] |= M_MORE;
            memcpy(b + n, d, first);
            d += first;
        } else if (i < parts) {
            b[0] = M_MORE;
            memcpy(b + 2, d, CYR_LONG_BODY - 2);
            d += CYR_LONG_BODY - 2;
        } else {
            b[0] = M_MORE | M_LAST;
            b[2] = (uint8_t)rest;
            memcpy(b + 3, d, rest);
        }
        cyr_seal(p, CYR_LONG);
    }
    return parts;
}

bool cyr_msg_parse(const uint8_t *body, size_t len, cyr_msg_t *m)
{
    memset(m, 0, sizeof *m);
    if (len < 6) return false;
    const uint8_t flags = body[0];
    size_t n = 4;
    m->extra = body[1];
    m->id = get16(body + 2);
    if (flags & M_P0) { if (n + 4 > len) return false; m->has_p0 = true; m->p0 = get32(body + n); n += 4; }
    if (flags & M_P1) { if (n + 4 > len) return false; m->has_p1 = true; m->p1 = get32(body + n); n += 4; }
    if (flags & M_NAME) {
        size_t l = strnlen((const char *)body + n, len - n);
        if (n + l >= len) return false;
        m->name = (const char *)body + n;
        n += l + 1;
    } else {
        if (n + 2 > len) return false;
        m->port = get16(body + n);
        n += 2;
    }
    if (flags & M_DATA) {
        size_t dl;
        if (flags & M_MORE) {
            dl = len - n;                      /* put together from parts */
        } else {
            if (n >= len) return false;
            dl = body[n++];
            if (n + dl > len) return false;
        }
        m->data = body + n;
        m->len = dl;
    }
    return true;
}

int cyr_rx_feed(cyr_rx_t *rx, const uint8_t *pkt, size_t len, uint8_t *out)
{
    const uint8_t *b = pkt + CYR_HEAD;
    const size_t bl = cyr_body_len(len);
    const uint32_t from = cyr_from(pkt);
    const uint8_t part = pkt[10], number = pkt[11];
    if (part == 0) {
        memcpy(out, b, bl);
        return (int)bl;
    }
    int k = -1;
    for (int i = 0; i < 4; i++)
        if (rx->s[i].next && rx->s[i].from == from && rx->s[i].number == number) k = i;
    if (part == 1) {
        if (k < 0) { k = rx->victim; rx->victim = (rx->victim + 1) % 4; }
        rx->s[k].from = from;
        rx->s[k].number = number;
        rx->s[k].next = 2;
        memcpy(rx->s[k].buf, b, bl);
        rx->s[k].len = bl;
        return 0;
    }
    if (k < 0 || rx->s[k].next != part) return -1;
    const bool last = b[0] & M_LAST;
    size_t dl = last ? b[2] : bl - 2;
    const uint8_t *d = b + (last ? 3 : 2);
    if (d + dl > b + bl) dl = (size_t)(b + bl - d);
    if (rx->s[k].len + dl > CYR_MSG_MAX) { rx->s[k].next = 0; return -1; }
    memcpy(rx->s[k].buf + rx->s[k].len, d, dl);
    rx->s[k].len += dl;
    rx->s[k].next++;
    if (!last) return 0;
    rx->s[k].next = 0;
    memcpy(out, rx->s[k].buf, rx->s[k].len);
    return (int)rx->s[k].len;
}

/* ---- chat ---- */

void cyr_chat(cyr_msg_t *m, int room, uint32_t clock, const char *text, bool private_line)
{
    memset(m, 0, sizeof *m);
    m->id = CYR_MSG_CHAT;
    m->extra = private_line ? 0x80 : 0;
    m->p0 = clock ? clock : 1;
    m->p1 = room > 0 ? (uint32_t)room : 0;
    m->port = CYR_PORT_CHAT;
    m->data = (const uint8_t *)text;
    size_t l = strlen(text);
    m->len = (l > CYR_CHAT_MAX ? CYR_CHAT_MAX : l) + 1;
}

bool cyr_chat_parse(const cyr_msg_t *m, int *room, const char **text, size_t *text_len)
{
    if (m->id != CYR_MSG_CHAT || m->name || m->port != CYR_PORT_CHAT || !m->data) return false;
    if (room) *room = (int)m->p1;
    *text = (const char *)m->data;
    *text_len = strnlen(*text, m->len);
    return true;
}

/* ---- names ---- */

void cyr_cyid_str(uint32_t id, char *out)
{
    static const char alpha[] = "ABCDEFGHKLMNPRSTUVWXYZQJ23456789";
    if (id == 0 || id >= 0xfffffffeu) {
        memcpy(out, "UNKNOWN", 8);
        return;
    }
    out[6] = alpha[(id & 1) + (id >> 31 ? 2 : 0)];
    uint32_t v = id >> 1;
    for (int i = 5; i >= 0; i--) {
        out[i] = alpha[v & 31];
        v >>= 5;
    }
    out[7] = 0;
}

/* ---- ESP-NOW ---- */

size_t cyr_espnow_wrap(uint8_t *frame, const uint8_t *pkt, size_t len, uint8_t channel)
{
    frame[0] = 'C';
    frame[1] = 'Y';
    frame[2] = 2;
    frame[3] = channel;
    memcpy(frame + CYR_ESPNOW_HEAD, pkt, len);
    return CYR_ESPNOW_HEAD + len;
}

size_t cyr_espnow_unwrap(const uint8_t *frame, size_t len, const uint8_t **pkt, uint8_t *channel)
{
    if (len < CYR_ESPNOW_HEAD || frame[0] != 'C' || frame[1] != 'Y' || frame[2] != 2) return 0;
    len -= CYR_ESPNOW_HEAD;
    if (len != CYR_SHORT && len != CYR_LONG) return 0;
    *pkt = frame + CYR_ESPNOW_HEAD;
    *channel = frame[3];
    return len;
}
