#ifndef NETWORK_H
#define NETWORK_H

#include "protocol.h"

int init_listener(int port);
int accept_connection(int listener_fd, char *peer_ip, char *peer_nick);
int connect_to_peer(const char *ip, int port, const char *my_nickname);
int send_conn_accept(int sock_fd);
int send_conn_reject(int sock_fd);

#endif
