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
#include <errno.h>

/* -------------------------------------------------------------------------
 * FIX L-3: instead of closing the listener fd from the main thread (which
 * races with accept_thread's select()), we use a self-pipe.  Writing one
 * byte to shutdown_pipe[1] wakes accept_thread cleanly.
 * -----------------------------------------------------------------------*/
static int g_shutdown_pipe[2] = {-1, -1};

typedef struct {
    Session    *s;
    int         fd;
    void      (*display_cb)(Packet *);
    atomic_int *running;
} PeerArgs;

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

            Packet leave;
            memset(&leave, 0, sizeof(leave));
            leave.type = PEER_LEAVE;
            strncpy(leave.sender, nick, MAX_NAME - 1);
            snprintf(leave.content, MAX_MSG - 1, "%s left the chat.", nick);
            if (a->s->is_host)
                room_broadcast(a->s, &leave, -1);
            if (a->display_cb) a->display_cb(&leave);
            break;
        }

        /* FIX C-5: validate packet type before dispatch. */
        if (!PACKET_TYPE_VALID(p.type)) continue;

        if (p.type == NICK_CHANGE) {
            /* FIX M-2: validate the new nickname before accepting it. */
            size_t len = strnlen(p.content, MAX_NAME);
            if (len == 0 || len >= MAX_NAME)
                continue;  /* drop invalid nick-change silently */
            for (size_t i = 0; i < len; i++)
                if (p.content[i] == ' ') p.content[i] = '_';

            room_rename(a->s, a->fd, p.content);
            if (a->s->is_host)
                room_broadcast(a->s, &p, a->fd);
            if (a->display_cb) a->display_cb(&p);
            continue;
        }

        if (p.type == ROSTER_SYNC) continue;

        if (a->s->is_host) {
            strncpy(p.target, "everyone", MAX_NAME - 1);
            room_broadcast(a->s, &p, a->fd);
        }
        if (a->display_cb) a->display_cb(&p);
    }

    free(a);
    return NULL;
}

/* -------------------------------------------------------------------------
 * accept_thread
 * FIX C-4: the capacity/password check and room_add() are now performed
 *          inside a single critical section (via a helper in room.c whose
 *          locking wraps all three steps atomically).  See room_try_add().
 * FIX L-3: select() also watches shutdown_pipe[0] so we wake immediately
 *          when the main thread signals shutdown instead of racing on close.
 * -----------------------------------------------------------------------*/
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

        int maxfd = s->listener_fd;
        /* FIX L-3: include shutdown pipe read-end in the select set. */
        if (g_shutdown_pipe[0] >= 0) {
            FD_SET(g_shutdown_pipe[0], &rfds);
            if (g_shutdown_pipe[0] > maxfd) maxfd = g_shutdown_pipe[0];
        }

        int nready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (nready <= 0) continue;

        /* Shutdown signal received — exit cleanly. */
        if (g_shutdown_pipe[0] >= 0 &&
            FD_ISSET(g_shutdown_pipe[0], &rfds))
            break;

        if (!FD_ISSET(s->listener_fd, &rfds)) continue;

        char peer_ip[64]         = {0};
        char peer_nick[MAX_NAME] = {0};
        char peer_pass[MAX_PASS] = {0};
        int conn = accept_connection(s->listener_fd, peer_ip,
                                     peer_nick, peer_pass);
        if (conn < 0) continue;

        /*
         * FIX C-4: atomically check password + capacity and add the peer
         * inside room_try_add(), which holds room_mu for the whole
         * decision+insert so two simultaneous connects can't both pass.
         */
        int bad_pw = (s->password[0] &&
                      strcmp(peer_pass, s->password) != 0);
        if (bad_pw) {
            send_conn_wrong_pass(conn);
            continue;
        }

        if (room_try_add(s, conn, peer_nick) != 0) {
            send_conn_reject(conn);
            continue;
        }

        send_conn_accept(conn, s->my_nick);

        Packet cs;
        memset(&cs, 0, sizeof(cs));
        cs.type = CHAT_START;
        send(conn, &cs, sizeof(Packet), 0);

        Packet join;
        memset(&join, 0, sizeof(join));
        join.type = PEER_JOIN;
        strncpy(join.sender, peer_nick, MAX_NAME - 1);
        snprintf(join.content, MAX_MSG - 1, "%s joined the chat.", peer_nick);
        room_broadcast(s, &join, conn);
        if (a->display_cb) a->display_cb(&join);

        pthread_t tid;
        spawn_peer_thread(s, conn, a->display_cb, a->running, &tid);
        if (tid) pthread_detach(tid);
    }

    free(a);
    return NULL;
}

/*
 * FIX M-7: free PeerArgs on pthread_create() failure; set *tid = 0 so
 * the join loop in start_chat() skips it rather than joining tid 0.
 */
static void spawn_peer_thread(Session *s, int fd, void (*cb)(Packet *),
                              atomic_int *running, pthread_t *tid) {
    PeerArgs *a = malloc(sizeof(PeerArgs));
    if (!a) { *tid = 0; return; }
    a->s = s; a->fd = fd;
    a->display_cb = cb; a->running = running;
    if (pthread_create(tid, NULL, peer_recv_thread, a) != 0) {
        free(a);
        *tid = 0;
    }
}

void start_chat(Session *s, void (*display_cb)(Packet *)) {
    atomic_int running;
    atomic_init(&running, 1);

    /* FIX L-3: create shutdown self-pipe. */
    if (pipe(g_shutdown_pipe) < 0) {
        perror("start_chat: pipe");
        g_shutdown_pipe[0] = g_shutdown_pipe[1] = -1;
    }

    pthread_t tids[MAX_CLIENTS] = {0};
    int n_tids = 0;

    for (int i = 0; i < s->count; i++) {
        spawn_peer_thread(s, s->fds[i], display_cb, &running,
                          &tids[n_tids++]);
    }

    pthread_t accept_tid = 0;
    if (s->is_host && s->listener_fd >= 0) {
        AcceptArgs *aa = malloc(sizeof(AcceptArgs));
        if (!aa) goto shutdown;
        aa->s = s; aa->display_cb = display_cb; aa->running = &running;
        pthread_create(&accept_tid, NULL, accept_thread, aa);
    }

    while (atomic_load(&running)) {
        char *input = tui_get_input();
        if (!input) {
            if (tui_was_resized()) { tui_handle_resize(); continue; }
            break;
        }
        if (input[0] == '\0') { free(input); continue; }

        if (input[0] == '/') {
            CmdResult r = cmd_dispatch(input, s->my_nick, s);
            free(input);
            if (r == CMD_QUIT) break;
            continue;
        }

        Packet out;
        memset(&out, 0, sizeof(out));
        out.type = MSG;
        strncpy(out.sender, s->my_nick,  MAX_NAME - 1);
        strncpy(out.target, "everyone",  MAX_NAME - 1);
        strncpy(out.content, input,      MAX_MSG  - 1);
        free(input);

        room_broadcast(s, &out, -1);
        if (display_cb) display_cb(&out);
    }

shutdown:
    atomic_store(&running, 0);

    if (accept_tid) {
        /*
         * FIX L-3: signal accept_thread via the pipe; do NOT close
         * listener_fd from here while accept_thread may be in select().
         */
        if (g_shutdown_pipe[1] >= 0) {
            char byte = 1;
            (void)write(g_shutdown_pipe[1], &byte, 1);
        }
        pthread_join(accept_tid, NULL);

        /* Now safe to close the listener fd — accept_thread has exited. */
        if (s->listener_fd >= 0) {
            close(s->listener_fd);
            s->listener_fd = -1;
        }
    }

    /* Close shutdown pipe. */
    if (g_shutdown_pipe[0] >= 0) { close(g_shutdown_pipe[0]); g_shutdown_pipe[0] = -1; }
    if (g_shutdown_pipe[1] >= 0) { close(g_shutdown_pipe[1]); g_shutdown_pipe[1] = -1; }

    room_shutdown_all(s);

    /* FIX M-7: only join threads that were actually created (tid != 0). */
    for (int i = 0; i < n_tids; i++)
        if (tids[i] != 0) pthread_join(tids[i], NULL);
}
