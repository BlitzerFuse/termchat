#ifndef ROOM_H
#define ROOM_H

#include "session.h"
#include "protocol.h"
#include <stddef.h>

void room_broadcast(Session *s, Packet *p, int skip_fd);
void room_remove(Session *s, int fd);
int  room_add(Session *s, int fd, const char *nick);
void room_nick_for_fd(Session *s, int fd, char *out, size_t len);
void room_shutdown_all(Session *s);
void room_rename(Session *s, int fd, const char *new_nick);

#endif
