#include "tui.h"
#include "tui_internal.h"
#include "session.h"
#include "network.h"
#include "room.h"
#include "protocol.h"
#include <ncurses.h>
#include <sys/select.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>

#define LOBBY_W      58
#define LOBBY_H      26
#define PEER_ROW0    14
#define PEER_ROWS    MAX_CLIENTS

static time_t g_join_times[MAX_CLIENTS];

static void lobby_section(WINDOW *w, int row, const char *label) {
    mvwaddch(w, row, 0,         ACS_LTEE);
    mvwhline(w, row, 1,         ACS_HLINE, LOBBY_W - 2);
    mvwaddch(w, row, LOBBY_W-1, ACS_RTEE);
    if (label && label[0])
        mvwprintw(w, row, 2, " %s ", label);
}

/*
 * FIX M-8: lobby_draw_peers() acquires room_mu before reading s->count
 * and s->nicks[], matching the locking discipline in room.c and closing
 * the latent race that existed if the lobby were ever made concurrent.
 */
static void lobby_draw_peers(WINDOW *w, Session *s) {
    /* Take a local snapshot under the lock. */
    int   count;
    char  nicks[MAX_CLIENTS][MAX_NAME];

    pthread_mutex_lock(&room_mu);
    count = s->count;
    for (int i = 0; i < count && i < MAX_CLIENTS; i++)
        memcpy(nicks[i], s->nicks[i], MAX_NAME);
    pthread_mutex_unlock(&room_mu);

    char label[40];
    snprintf(label, sizeof(label), "connected  %d / %d",
             count, MAX_CLIENTS);
    lobby_section(w, 12, label);

    for (int r = PEER_ROW0; r < PEER_ROW0 + PEER_ROWS; r++) {
        wmove(w, r, 1); wclrtoeol(w);
    }

    if (count == 0) {
        mvwprintw(w, PEER_ROW0, 4, "(no peers yet)");
    } else {
        for (int i = 0; i < count && i < PEER_ROWS; i++) {
            char tbuf[8] = "--:--";
            if (g_join_times[i]) {
                struct tm *t = localtime(&g_join_times[i]);
                strftime(tbuf, sizeof(tbuf), "%H:%M", t);
            }
            mvwprintw(w, PEER_ROW0 + i, 4,
                      "%-24.24s  %s", nicks[i], tbuf);
        }
    }
    wrefresh(w);
}

static void lobby_draw_all(WINDOW *w, Session *s,
                            const char *password,
                            const char *local_ip,
                            int port) {
    werase(w);
    box(w, 0, 0);

    mvwprintw(w, 2, (LOBBY_W - 9)  / 2, "term-chan");
    mvwprintw(w, 3, (LOBBY_W - 11) / 2, "create room");

    lobby_section(w, 5, NULL);

    mvwprintw(w, 7,  4, "host        %s", s->my_nick);
    if (password && password[0])
        mvwprintw(w, 8,  4,
                  "password    %s   share with peers", password);
    else
        mvwprintw(w, 8,  4, "password    none");
    mvwprintw(w, 9,  4, "port        %d", port);
    mvwprintw(w, 10, 4, "address     %s", local_ip);

    lobby_draw_peers(w, s);

    lobby_section(w, 22, NULL);
    mvwprintw(w, 23, 4, "waiting for peers...");
    lobby_section(w, 24, NULL);
    mvwprintw(w, 25, 4, "enter  start chat     q  quit");

    wrefresh(w);
}

int tui_lobby(Session *s, int listener_fd, const char *password, int port) {
    ncurses_start();
    memset(g_join_times, 0, sizeof(g_join_times));

    char local_ip[64];
    tui_get_local_ip(local_ip, sizeof(local_ip));

    int bx = (COLS - LOBBY_W) / 2;
    int by = (LINES - LOBBY_H) / 2;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;

    WINDOW *w = newwin(LOBBY_H, LOBBY_W, by, bx);
    keypad(w, TRUE);
    noecho();
    curs_set(0);

    lobby_draw_all(w, s, password, local_ip, port);

    while (1) {
        wtimeout(w, 0);
        int ch = wgetch(w);
        wtimeout(w, -1);

        if (ch == '\n' || ch == '\r') break;
        if (ch == 'q'  || ch == 27)  { delwin(w); endwin(); return -1; }

        fd_set rfds;
        struct timeval tv = {0, 200000};
        FD_ZERO(&rfds);
        FD_SET(listener_fd, &rfds);
        if (select(listener_fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            continue;

        char peer_ip[64]         = {0};
        char peer_nick[MAX_NAME] = {0};
        char peer_pass[MAX_PASS] = {0};
        int conn = accept_connection(listener_fd, peer_ip,
                                     peer_nick, peer_pass);
        if (conn < 0) continue;

        if (password && password[0] &&
            strcmp(peer_pass, password) != 0) {
            send_conn_wrong_pass(conn);
            continue;
        }

        /*
         * FIX M-8 / C-4: use room_try_add() so the capacity check and
         * the insert are atomic under room_mu — no TOCTOU window.
         */
        if (!tui_accept_request(peer_nick, peer_ip)) {
            send_conn_reject(conn);
            lobby_draw_all(w, s, password, local_ip, port);
            continue;
        }

        if (room_try_add(s, conn, peer_nick) != 0) {
            send_conn_reject(conn);
            lobby_draw_all(w, s, password, local_ip, port);
            continue;
        }

        send_conn_accept(conn, s->my_nick);

        /*
         * FIX M-3: snapshot the index BEFORE room_try_add() increments
         * s->count, then guard it to prevent out-of-bounds writes to
         * g_join_times[].  room_try_add() already incremented count, so
         * the slot we just filled is at count-1; we captured idx before
         * the call, which equals the old count value == the new index.
         *
         * Implementation note: room_try_add() increments s->count
         * atomically inside room_mu, so the value we read from s->count
         * after the call is the NEW count.  The index of the entry just
         * added is therefore (s->count - 1).  We guard it explicitly.
         */
        {
            pthread_mutex_lock(&room_mu);
            int idx = s->count - 1;
            pthread_mutex_unlock(&room_mu);
            if (idx >= 0 && idx < MAX_CLIENTS)
                g_join_times[idx] = time(NULL);
        }

        /*
         * FIX L-1: build ROSTER_SYNC content BEFORE including the new
         * peer's own nick, so they don't receive their own name twice
         * (the waiting screen already adds them at index 1 from my_nick).
         *
         * We iterate s->nicks[] but skip the slot whose nick matches
         * peer_nick (the one just added at count-1).
         *
         * FIX M-8: read s->count and s->nicks[] under room_mu.
         */
        Packet rsync;
        memset(&rsync, 0, sizeof(rsync));
        rsync.type = ROSTER_SYNC;
        strncpy(rsync.sender, s->my_nick, MAX_NAME - 1);

        char *rp   = rsync.content;
        int   rrem = MAX_MSG - 1;

        /* Always include the host's own nick first. */
        int n = snprintf(rp, rrem, "%s\n", s->my_nick);
        if (n > 0 && n < rrem) { rp += n; rrem -= n; }

        pthread_mutex_lock(&room_mu);
        int snap_count = s->count;
        char snap_nicks[MAX_CLIENTS][MAX_NAME];
        for (int i = 0; i < snap_count && i < MAX_CLIENTS; i++)
            memcpy(snap_nicks[i], s->nicks[i], MAX_NAME);
        pthread_mutex_unlock(&room_mu);

        for (int i = 0; i < snap_count && rrem > 1; i++) {
            /*
             * FIX L-1: skip the newly-added peer's own nick so it does
             * not appear twice in the waiting-room list.
             */
            if (strcmp(snap_nicks[i], peer_nick) == 0) continue;
            /* Also skip the host (already added above). */
            if (strcmp(snap_nicks[i], s->my_nick) == 0) continue;
            n = snprintf(rp, rrem, "%s\n", snap_nicks[i]);
            if (n > 0 && n < rrem) { rp += n; rrem -= n; }
        }
        rsync.content[MAX_MSG - 1] = '\0';
        send(conn, &rsync, sizeof(Packet), 0);

        Packet join;
        memset(&join, 0, sizeof(join));
        join.type = PEER_JOIN;
        strncpy(join.sender, peer_nick, MAX_NAME - 1);
        snprintf(join.content, MAX_MSG - 1, "%s joined the room.",
                 peer_nick);
        room_broadcast(s, &join, conn);

        lobby_draw_all(w, s, password, local_ip, port);
    }

    delwin(w);
    return 0;
}
