/* Chat over the Internet: a seat in a room of the radio relay that the
 * Cybiko web emulator uses (https://github.com/SovGVD/cybiko-web-emulator,
 * proxy/). The relay passes radio packets between everyone in a room as
 * binary WebSocket messages: sender tab id (4), sequence number (4), RF
 * channel (1), then the 50- or 200-byte packet (cy_radio.h).
 *
 * This side speaks just enough of the radio protocol to chat: it hears the
 * room's chat lines (and the nicknames in the pings), and says lines in the
 * name of any Cybiko ID, so a real Cybiko's lines show up in the web
 * emulators as that Cybiko's. It pings as a Cybiko of its own (my_id, with
 * the nickname given) so the room sees that the bridge is there. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct relay relay_t;

typedef struct {
    void *user;
    /* a chat line heard in the room: sender, their nickname ("" when no
     * ping told it), room code, text */
    void (*line)(void *user, uint32_t from, const char *nick, int room, const char *text);
    void (*log)(void *user, const char *line);    /* called from the relay's thread too */
} relay_host_t;

/* url: the relay's address (https://... or wss://...), room: the room's
 * name. A thread of its own connects (again after a drop) and talks to the
 * room; lines heard are handed over in relay_poll. */
relay_t *relay_create(const char *url, const char *room, const char *origin, uint32_t my_id, const char *nick,
                      const relay_host_t *host);
void relay_destroy(relay_t *r);

/* Calls host.line for the lines heard since the last call. */
void relay_poll(relay_t *r);
bool relay_online(const relay_t *r);

/* Say a chat line to everyone in a room (CYR_ROOM_*) as Cybiko from;
 * clock tells the line's copies apart (CyOS's clock() for the line). */
void relay_say(relay_t *r, uint32_t from, int room, uint32_t clock, const char *text);
