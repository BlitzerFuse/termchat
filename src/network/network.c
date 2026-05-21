#include "network.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * FIX C-3: accept_connection()
 *   - Timeout is cleared with {0,0} (both fields) after the handshake recv.
 *   - errno is checked after a failed recv() so that a timed-out handshake
 *     (EAGAIN/EWOULDBLOCK) is logged distinctly from a hard socket error,
 *     aiding diagnosis of the low-cost DoS attack vector.
 *
 * FIX M-1: connect_to_peer()
 *   - A 5-second SO_RCVTIMEO is now set before the response recv() and
 *     cleared afterwards.  A malicious or crashed host can no longer hang
 *     the connecting client indefinitely.
 * -----------------------------------------------------------------------*/

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
    if (listen(sock, 5) < 0) { perror("listen"); close(sock); return -1; }
    return sock;
}

int accept_connection(int listener_fd, char *peer_ip,
                      char *peer_nick, char *peer_pass) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int conn = accept(listener_fd, (struct sockaddr *)&addr, &len);
    if (conn < 0) { perror("accept"); return -1; }
    if (peer_ip) strncpy(peer_ip, inet_ntoa(addr.sin_addr), 63);

    /* FIX C-3: 2-second timeout for the handshake recv. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    Packet p;
    ssize_t n = recv(conn, &p, sizeof(Packet), 0);

    /* FIX C-3: clear timeout — BOTH fields must be zero. */
    struct timeval no_tv = { .tv_sec = 0, .tv_usec = 0 };
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &no_tv, sizeof(no_tv));

    if (n <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            fprintf(stderr,
                    "accept_connection: handshake timed out from %s\n",
                    peer_ip ? peer_ip : "unknown");
        else
            fprintf(stderr,
                    "accept_connection: recv error from %s: %s\n",
                    peer_ip ? peer_ip : "unknown", strerror(errno));
        close(conn);
        return -1;
    }

    /* FIX C-5: validate packet type before using it. */
    if (!PACKET_TYPE_VALID(p.type) || p.type != CONN_REQUEST) {
        close(conn);
        return -1;
    }

    if (peer_nick) strncpy(peer_nick, p.sender,   MAX_NAME - 1);
    if (peer_pass) strncpy(peer_pass, p.password, MAX_PASS - 1);
    return conn;
}

int send_conn_accept(int sock_fd, const char *my_nick) {
    Packet p;
    memset(&p, 0, sizeof(p));
    p.type = CONN_ACCEPT;
    if (my_nick) strncpy(p.sender, my_nick, MAX_NAME - 1);
    return send(sock_fd, &p, sizeof(Packet), 0) > 0 ? 0 : -1;
}

int send_conn_reject(int sock_fd) {
    Packet p;
    memset(&p, 0, sizeof(p));
    p.type = CONN_REJECT;
    send(sock_fd, &p, sizeof(Packet), 0);
    close(sock_fd);
    return 0;
}

int send_conn_wrong_pass(int sock_fd) {
    Packet p;
    memset(&p, 0, sizeof(p));
    p.type = CONN_WRONG_PASS;
    send(sock_fd, &p, sizeof(Packet), 0);
    close(sock_fd);
    return 0;
}

int connect_to_peer(const char *ip, int port, const char *my_nickname,
                    const char *password, char *host_nick_out) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return NET_ERR_GENERIC; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port)
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(sock); return NET_ERR_GENERIC;
    }

    Packet req;
    memset(&req, 0, sizeof(req));
    req.type = CONN_REQUEST;
    strncpy(req.sender,   my_nickname,            MAX_NAME - 1);
    strncpy(req.password, password ? password : "", MAX_PASS - 1);

    if (send(sock, &req, sizeof(Packet), 0) <= 0) {
        close(sock); return NET_ERR_GENERIC;
    }

    /* FIX M-1: 5-second timeout so a silent/crashed host can't hang us. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    Packet resp;
    int n = recv(sock, &resp, sizeof(Packet), 0);

    struct timeval no_tv = { .tv_sec = 0, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &no_tv, sizeof(no_tv));

    if (n <= 0) { close(sock); return NET_ERR_GENERIC; }

    /* FIX C-5: validate packet type. */
    if (!PACKET_TYPE_VALID(resp.type)) { close(sock); return NET_ERR_GENERIC; }

    if (resp.type == CONN_WRONG_PASS) { close(sock); return NET_ERR_WRONGPASS; }
    if (resp.type == CONN_REJECT)     { close(sock); return NET_ERR_REJECTED;  }
    if (resp.type != CONN_ACCEPT)     { close(sock); return NET_ERR_GENERIC;   }

    if (host_nick_out) strncpy(host_nick_out, resp.sender, MAX_NAME - 1);
    return sock;
}
