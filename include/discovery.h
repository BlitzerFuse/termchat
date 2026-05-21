#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "protocol.h"

#define DISCOVERY_PORT 5051
#define MAX_PEERS      16

typedef struct {
    char nickname[MAX_NAME];
    char ip[64];
} Peer;

void discovery_start(const char *my_nickname, int port);
void discovery_stop(void);

/*
 * FIX L-6: update the nickname advertised in UDP beacons while the beacon
 * thread is running.  Call this from handle_nick() after changing the
 * session nick so that peers who rescan see the new name immediately.
 */
void discovery_set_nick(const char *new_nick);

int  discovery_peers(Peer *peers, int max);
void discovery_reset(void);

#endif
