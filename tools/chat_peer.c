/* chat_peer: a Cybiko of its own in a room of the radio relay, for trying
 * the chat bridge without a web browser. It prints the lines it hears and
 * says the ones given.
 *
 *   build/chat_peer [--relay URL] [--room NAME] [--nick NAME] [--seconds S]
 *                   [--say MS:TEXT]... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cy_radio.h"
#include "relay.h"
#include <openssl/rand.h>

static void line(void *user, uint32_t from, const char *nick, int room, const char *text)
{
    char id[8];
    (void)user;
    cyr_cyid_str(from, id);
    printf("heard %s (@%s) in room %d: %s\n", *nick ? nick : "?", id, room, text);
    fflush(stdout);
}

static void say_log(void *user, const char *s) { (void)user; fprintf(stderr, "%s\n", s); }

static unsigned ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
    const char *url = "https://cybiko-relay.sovgvd.workers.dev", *room = "public", *nick = "Peer";
    struct { unsigned at; const char *text; int done; } say[16];
    int nsay = 0;
    double seconds = 60;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--relay")) url = argv[i + 1];
        else if (!strcmp(argv[i], "--room")) room = argv[i + 1];
        else if (!strcmp(argv[i], "--nick")) nick = argv[i + 1];
        else if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--say") && nsay < 16) {
            char *e;
            say[nsay].at = (unsigned)strtoul(argv[i + 1], &e, 10);
            say[nsay].text = *e == ':' ? e + 1 : e;
            say[nsay++].done = 0;
        } else { fprintf(stderr, "usage: see tools/chat_peer.c\n"); return 2; }
    }
    relay_host_t h = { NULL, line, say_log };
    uint32_t id;
    RAND_bytes((unsigned char *)&id, sizeof id);
    id = 0x4c000000u | (id & 0xffffff);
    relay_t *r = relay_create(url, room, "https://sovgvd.github.io", id, nick, &h);
    unsigned t0 = ms();
    while (ms() - t0 < seconds * 1000) {
        for (int i = 0; i < nsay; i++)
            if (!say[i].done && ms() - t0 >= say[i].at) {
                say[i].done = 1;
                printf("says: %s\n", say[i].text);
                fflush(stdout);
                relay_say(r, id, 0, ms() - t0 + 1, say[i].text);
            }
        relay_poll(r);
        struct timespec ts = { 0, 50 * 1000000 };
        nanosleep(&ts, NULL);
    }
    relay_destroy(r);
    return 0;
}
