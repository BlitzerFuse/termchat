#ifndef SESSION_H
#define SESSION_H

#include "protocol.h"

#define MAX_CLIENTS 7

/*
 * Session — live connection state for one termchan run.
 *
 * NOTE (L-10 / security): the password field holds the plaintext session
 * password for its entire lifetime.  Call explicit_bzero() on both this
 * struct and MenuResult before returning from main() to prevent the secret
 * from lingering in core dumps or swap.
 *
 * NOTE: all multi-field reads/writes to fds[], nicks[], and count MUST be
 * performed while holding room_mu (defined in room.c) to avoid the TOCTOU
 * race documented in audit findings C-4 and M-8.
 */
typedef struct {
    int  fds[MAX_CLIENTS];
    char nicks[MAX_CLIENTS][MAX_NAME];
    int  count;
    char my_nick[MAX_NAME];
    char password[MAX_PASS];
    int  is_host;
    int  listener_fd;
} Session;

#endif /* SESSION_H */
