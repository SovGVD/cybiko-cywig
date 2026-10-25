/* Copied from Dragonfruit (https://github.com/SovGVD/Dragonfruit-JC4827W543-esp32,
 * src/components/cy_radio); change it there first. */
/* The Cybiko radio protocol (CyOS 1.5 "CyDP") in plain C, for talking to
 * Cybikos from other programs: emulated ones over ESP-NOW, or anything
 * else that carries the radio's packets. No dependencies, no allocation.
 * The protocol is described in radio-protocol.md of cybiko-docs
 * (https://github.com/SovGVD/cybiko-docs); the ESP-NOW framing in
 * docs/cybiko-radio-protocol.md.
 *
 * Packets are 50 (short) or 200 (long) bytes: a 16-byte header, data, then
 * Reed-Solomon parity. All multi-byte fields are big-endian. */
#ifndef CY_RADIO_H
#define CY_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CYR_SHORT 50
#define CYR_LONG 200
#define CYR_HEAD 16
#define CYR_SHORT_BODY 14            /* data after the header */
#define CYR_LONG_BODY 104

#define CYR_BROADCAST 0xffffffffu

/* RF channels the radio tunes to (2..9). Base: where every Cybiko pings
 * and listens when no application has picked a channel; chat: where all
 * the chat rooms are (application channel 1). */
#define CYR_RF_BASE 2
#define CYR_RF_CHAT 4

/* Header byte 9: type in the top 3 bits, then flags */
#define CYR_T_PING 0x00
#define CYR_T_MSG 0x20
#define CYR_T_ACK 0x40
#define CYR_T_QUERY 0x60               /* "was it delivered?" */
#define CYR_F_ACK_ME 0x10
#define CYR_F_GATE 0x04                /* the sender is a CyWIG */
#define CYR_F_PING_DATA 0x01           /* the ping carries the owner's details */

/* Acknowledgement codes (top 4 bits of the first data byte) */
#define CYR_ACK_TAKEN 0                /* handed over (number 0, or a part) */
#define CYR_ACK_GOT 2                  /* arrived, delivery pending */
#define CYR_ACK_AGAIN 3                /* a repeat of one being worked on */
#define CYR_ACK_DELIVERED 4
#define CYR_ACK_NOBODY 5               /* no such process or port */

/* Well-known message ids and ports */
#define CYR_MSG_CHAT 0x1002            /* chat line, to port CYR_PORT_CHAT */
#define CYR_PORT_CHAT 0x0025
#define CYR_MSG_PACKED 0x0047          /* a message for a gate to pass on */
#define CYR_PORT_GATE 0x0093

/* Chat room codes (the chat message's p1). User-defined rooms are not
 * covered. */
enum { CYR_ROOM_GENERAL = 0, CYR_ROOM_ROMANCE = 1, CYR_ROOM_COMPUTING = 2,
       CYR_ROOM_SPORTS = 3, CYR_ROOM_GAMES = 4, CYR_ROOM_MUSIC = 5 };

#define CYR_CHAT_MAX 99                /* characters in a chat line */
#define CYR_MSG_MAX 1280               /* biggest message cyr_rx_t puts together (a gate part: 1024 bytes of data and the header) */

/* ---- packets ---- */

/* Fill in the header of a zeroed packet. channel is the sender's current
 * RF channel (byte 8 = c0 | channel). */
void cyr_header(uint8_t *pkt, uint32_t to, uint32_t from, uint8_t channel,
                uint8_t type_flags, uint8_t part, uint8_t number);
/* Compute both CRCs and the Reed-Solomon parity: call last. */
void cyr_seal(uint8_t *pkt, size_t len);
/* True if len is 50 or 200, the header is a CyDP one and both CRCs match
 * (the parity is not checked or used to correct errors). */
bool cyr_check(const uint8_t *pkt, size_t len);

static inline uint32_t cyr_to(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t cyr_from(const uint8_t *p) { return cyr_to(p + 4); }
static inline uint8_t cyr_type(const uint8_t *p) { return p[9] & 0xe0; }
static inline size_t cyr_body_len(size_t len) { return len == CYR_SHORT ? CYR_SHORT_BODY : CYR_LONG_BODY; }

/* A ping (beacon) to everyone. With nick != NULL it carries the owner's
 * details the way CyOS's Finder sets them: version (0 is skipped; change it
 * when the details change), nickname (up to 8 characters), age (-1 if not
 * given) and sex. Returns the packet length (CYR_SHORT). */
size_t cyr_ping(uint8_t *pkt, uint32_t from, uint8_t channel, uint8_t version,
                const char *nick, int age, bool female);
/* Decode a ping's details; false if it has none. nick gets 9 bytes. */
bool cyr_ping_details(const uint8_t *pkt, char *nick, int *age, bool *female);

/* The acknowledgement of a received message packet (or the answer to a
 * query) with the given code. Returns CYR_SHORT. */
size_t cyr_ack(uint8_t *pkt, const uint8_t *in, uint32_t from, uint8_t channel, int code);

/* Ask whether a message packet that was answered CYR_ACK_GOT has been
 * delivered (copies its part and number). The answer is an ack with
 * CYR_ACK_DELIVERED or CYR_ACK_NOBODY. Returns CYR_SHORT. */
size_t cyr_query(uint8_t *pkt, const uint8_t *sent, uint8_t channel);

/* ---- messages ---- */

typedef struct {
    uint16_t id;                     /* message id (msgid) */
    uint8_t extra;                   /* data byte 1: 80 on Chat's private lines */
    bool has_p0, has_p1;
    uint32_t p0, p1;
    const char *name;                /* target process name, or NULL for port */
    uint16_t port;
    const uint8_t *data;             /* NULL for none */
    size_t len;
} cyr_msg_t;

/* Split a message into packets. Up to max packets of CYR_LONG bytes go to
 * out (sealed); returns how many, or 0 if it does not fit. number is the
 * message number (0: none; with ack_me, the receiver answers at once). */
int cyr_msg_packets(const cyr_msg_t *m, uint32_t to, uint32_t from, uint8_t channel,
                    bool ack_me, uint8_t number, uint8_t out[][CYR_LONG], int max);
/* Read a whole message (the body of a single packet, or parts put together
 * by cyr_rx_t). Pointers in m point into body. */
bool cyr_msg_parse(const uint8_t *body, size_t len, cyr_msg_t *m);

/* Puts the parts of messages from up to 4 senders together. */
typedef struct {
    struct { uint32_t from; uint8_t number, next; size_t len; uint8_t buf[CYR_MSG_MAX]; } s[4];
    int victim;
} cyr_rx_t;
/* Feed a checked message packet (type CYR_T_MSG). Returns the length of a
 * finished message, copied to out (CYR_MSG_MAX bytes), or 0 while parts are
 * missing; -1 for a part out of order (it is dropped). */
int cyr_rx_feed(cyr_rx_t *rx, const uint8_t *pkt, size_t len, uint8_t *out);

/* ---- chat ---- */

/* A chat line for a room (CYR_ROOM_*). clock is the sender's clock in
 * milliseconds: receivers use it to tell repeats apart. Send it to
 * CYR_BROADCAST on CYR_RF_CHAT without ack_me, three times, or to one
 * Cybiko with ack_me and a message number. */
void cyr_chat(cyr_msg_t *m, int room, uint32_t clock, const char *text, bool private_line);
/* True if m is a chat line; gives its room and text (not NUL-ended past
 * text_len). */
bool cyr_chat_parse(const cyr_msg_t *m, int *room, const char **text, size_t *text_len);

/* ---- names ---- */

/* The 7-character text form of a Cybiko ID that CyOS shows (out gets 8
 * bytes), e.g. 4c8051a3 -> XEAMGVB. */
void cyr_cyid_str(uint32_t id, char *out);

/* ---- ESP-NOW (the Dragonfruit emulator's link) ----
 * Frame: 'C' 'Y' 2 rf-channel, then the 50- or 200-byte packet, broadcast
 * to ff:ff:ff:ff:ff:ff. */
#define CYR_ESPNOW_HEAD 4
size_t cyr_espnow_wrap(uint8_t *frame, const uint8_t *pkt, size_t len, uint8_t channel);
/* Returns the packet length (and sets *pkt, *channel), or 0 if the frame is
 * not one. */
size_t cyr_espnow_unwrap(const uint8_t *frame, size_t len, const uint8_t **pkt, uint8_t *channel);

#endif
