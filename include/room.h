#ifndef ROOM_H
#define ROOM_H

#include "session.h"
#include "protocol.h"
#include <stddef.h>
#include <pthread.h>

/*
 * room_mu is defined in room.c.  It is declared extern here so that
 * tui_lobby.c (FIX M-8) can acquire it when reading Session state without
 * going through a function call.
 */
extern pthread_mutex_t room_mu;

void room_broadcast(Session *s, Packet *p, int skip_fd);
void room_remove(Session *s, int fd);

/*
 * room_add — unconditional insert; use room_try_add() for the atomic
 * check+insert path needed in concurrent accept contexts.
 */
int  room_add(Session *s, int fd, const char *nick);

/*
 * FIX C-4: room_try_add() — capacity check and insert performed atomically
 * inside a single critical section.  Returns 0 on success, -1 if full.
 */
int  room_try_add(Session *s, int fd, const char *nick);

void room_nick_for_fd(Session *s, int fd, char *out, size_t len);
void room_shutdown_all(Session *s);
void room_rename(Session *s, int fd, const char *new_nick);

#endif /* ROOM_H */
