#ifndef SESSION_H
#define SESSION_H

#include "protocol.h"

#define MAX_CLIENTS 7

typedef struct {
    int  fds[MAX_CLIENTS];
    char nicks[MAX_CLIENTS][MAX_NAME];
    int  count;
    char my_nick[MAX_NAME];
    char password[MAX_PASS];
    int  is_host;
    int  listener_fd;
} Session;

#endif
