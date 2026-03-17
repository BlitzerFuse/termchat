#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "protocol.h"

#define DISCOVERY_PORT 5051
#define MAX_PEERS      16

typedef struct {
    char nickname[MAX_NAME];
    char ip[64];
} Peer;

/* Multicast a discover ping and collect responses for ~2s.
   Returns number of peers found, fills peers[]. */
int discover_peers(Peer *peers, int max, const char *my_nickname);

/* Join multicast group on DISCOVERY_PORT and reply to pings (blocking).
   Call this in a background thread while in listen mode. */
void discovery_respond(const char *my_nickname, volatile int *stop);

#endif
