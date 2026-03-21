#include "chat.h"
#include "room.h"
#include "commands.h"
#include "network.h"
#include "tui.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    Session    *s;
    int         fd;
    void      (*display_cb)(Packet *);
    atomic_int *running;
} PeerArgs;

/* Forward declaration so accept_thread can call it */
static void spawn_peer_thread(Session *s, int fd, void (*cb)(Packet *),
                              atomic_int *running, pthread_t *tid);

static void *peer_recv_thread(void *arg) {
    PeerArgs *a = arg;
    Packet p;

    while (atomic_load(a->running)) {
        if (recv(a->fd, &p, sizeof(Packet), 0) <= 0) {
            char nick[MAX_NAME];
            room_nick_for_fd(a->s, a->fd, nick, sizeof(nick));
            room_remove(a->s, a->fd);

            Packet leave = { .type = PEER_LEAVE };
            strncpy(leave.sender, nick, MAX_NAME - 1);
            snprintf(leave.content, MAX_MSG - 1, "%s left the chat.", nick);
            if (a->s->is_host)
                room_broadcast(a->s, &leave, -1);
            if (a->display_cb) a->display_cb(&leave);
            break;
        }

        if (p.type == NICK_CHANGE) {
            room_rename(a->s, a->fd, p.content);
            if (a->s->is_host)
                room_broadcast(a->s, &p, a->fd);
            if (a->display_cb) a->display_cb(&p);
            continue;
        }

        if (a->s->is_host) {
            strncpy(p.target, "everyone", MAX_NAME - 1);
            room_broadcast(a->s, &p, a->fd);
        }
        if (a->display_cb) a->display_cb(&p);
    }

    free(a);
    return NULL;
}

typedef struct {
    Session    *s;
    void      (*display_cb)(Packet *);
    atomic_int *running;
} AcceptArgs;

static void *accept_thread(void *arg) {
    AcceptArgs *a = arg;
    Session *s = a->s;

    while (atomic_load(a->running)) {
        if (s->listener_fd < 0) break;

        fd_set rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
        FD_ZERO(&rfds);
        FD_SET(s->listener_fd, &rfds);
        if (select(s->listener_fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            continue;

        char peer_ip[64]         = {0};
        char peer_nick[MAX_NAME] = {0};
        char peer_pass[MAX_PASS] = {0};
        int conn = accept_connection(s->listener_fd, peer_ip, peer_nick, peer_pass);
        if (conn < 0) continue;

        if (s->password[0] && strcmp(peer_pass, s->password) != 0) {
            send_conn_wrong_pass(conn);
            continue;
        }
        if (s->count >= MAX_CLIENTS) {
            send_conn_reject(conn);
            continue;
        }
        if (!tui_accept_request(peer_nick, peer_ip)) {
            send_conn_reject(conn);
            continue;
        }

        send_conn_accept(conn, s->my_nick);
        room_add(s, conn, peer_nick);

        /* Send CHAT_START so the new peer enters chat immediately */
        Packet cs = { .type = CHAT_START };
        send(conn, &cs, sizeof(Packet), 0);

        /* Announce to existing peers */
        Packet join = { .type = PEER_JOIN };
        strncpy(join.sender, peer_nick, MAX_NAME - 1);
        snprintf(join.content, MAX_MSG - 1, "%s joined the chat.", peer_nick);
        room_broadcast(s, &join, conn);
        if (a->display_cb) a->display_cb(&join);

        /* Spawn a recv thread for the new peer — reuse running flag */
        pthread_t tid;
        spawn_peer_thread(s, conn, a->display_cb, a->running, &tid);
        pthread_detach(tid);
    }

    free(a);
    return NULL;
}

static void spawn_peer_thread(Session *s, int fd, void (*cb)(Packet *),
                              atomic_int *running, pthread_t *tid) {
    PeerArgs *a = malloc(sizeof(PeerArgs));
    a->s = s; a->fd = fd;
    a->display_cb = cb; a->running = running;
    pthread_create(tid, NULL, peer_recv_thread, a);
}

void start_chat(Session *s, void (*display_cb)(Packet *)) {
    atomic_int running;
    atomic_init(&running, 1);

    pthread_t tids[MAX_CLIENTS] = {0};
    int n_tids = 0;

    for (int i = 0; i < s->count; i++) {
        spawn_peer_thread(s, s->fds[i], display_cb, &running, &tids[n_tids++]);
    }

    /* Host: keep accepting new peers during chat */
    pthread_t accept_tid = 0;
    if (s->is_host && s->listener_fd >= 0) {
        AcceptArgs *aa = malloc(sizeof(AcceptArgs));
        aa->s = s; aa->display_cb = display_cb; aa->running = &running;
        pthread_create(&accept_tid, NULL, accept_thread, aa);
    }

    while (atomic_load(&running)) {
        char *input = tui_get_input();
        if (!input) break;
        if (input[0] == '\0') { free(input); continue; }

        if (input[0] == '/') {
            CmdResult r = cmd_dispatch(input, s->my_nick, s);
            free(input);
            if (r == CMD_QUIT) break;
            continue;
        }

        Packet out = { .type = MSG };
        strncpy(out.sender, s->my_nick, MAX_NAME - 1);
        strncpy(out.target, "everyone", MAX_NAME - 1);
        strncpy(out.content, input, MAX_MSG - 1);
        free(input);

        room_broadcast(s, &out, -1);
        if (display_cb) display_cb(&out);
    }

    atomic_store(&running, 0);

    /* Stop accept thread by closing the listener */
    if (accept_tid) {
        if (s->listener_fd >= 0) {
            close(s->listener_fd);
            s->listener_fd = -1;
        }
        pthread_join(accept_tid, NULL);
    }

    room_shutdown_all(s);
    for (int i = 0; i < n_tids; i++)
        pthread_join(tids[i], NULL);
}
