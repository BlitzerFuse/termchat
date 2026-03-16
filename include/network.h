#ifndef NETWORK_H
#define NETWORK_H

#include "protocol.h"

int init_listener(int port);
int accept_connection(int listener_fd, char *peer_ip, char *peer_nick, char *peer_pass);
int connect_to_peer(const char *ip, int port, const char *my_nickname, const char *password);
int send_conn_accept(int sock_fd);
int send_conn_reject(int sock_fd);
int send_conn_wrong_pass(int sock_fd);

#endif
