#include "network.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int init_listener(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port)
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sock); return -1;
    }
    if (listen(sock, 5) < 0) {
        perror("listen"); close(sock); return -1;
    }
    return sock;
}

int accept_connection(int listener_fd, char *peer_ip, char *peer_nick) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int conn = accept(listener_fd, (struct sockaddr *)&addr, &len);
    if (conn < 0) { perror("accept"); return -1; }
    if (peer_ip) strncpy(peer_ip, inet_ntoa(addr.sin_addr), 63);

    /* Wait for the CONN_REQUEST handshake packet */
    Packet p;
    if (recv(conn, &p, sizeof(Packet), 0) <= 0 || p.type != CONN_REQUEST) {
        close(conn);
        return -1;
    }
    if (peer_nick) strncpy(peer_nick, p.sender, MAX_NAME - 1);

    return conn;
}

int send_conn_accept(int sock_fd) {
    Packet p = { .type = CONN_ACCEPT };
    return send(sock_fd, &p, sizeof(Packet), 0) > 0 ? 0 : -1;
}

int send_conn_reject(int sock_fd) {
    Packet p = { .type = CONN_REJECT };
    send(sock_fd, &p, sizeof(Packet), 0);
    close(sock_fd);
    return 0;
}

int connect_to_peer(const char *ip, int port, const char *my_nickname) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port)
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(sock); return -1;
    }

    /* Send connection request with our nickname */
    Packet req = { .type = CONN_REQUEST };
    strncpy(req.sender, my_nickname, MAX_NAME - 1);
    if (send(sock, &req, sizeof(Packet), 0) <= 0) {
        close(sock); return -1;
    }

    /* Wait for accept or reject */
    Packet resp;
    if (recv(sock, &resp, sizeof(Packet), 0) <= 0) {
        close(sock); return -1;
    }
    if (resp.type == CONN_REJECT) {
        close(sock); return -2;  /* -2 = explicitly rejected */
    }
    if (resp.type != CONN_ACCEPT) {
        close(sock); return -1;
    }

    return sock;
}
