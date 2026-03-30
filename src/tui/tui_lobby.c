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
    mvwaddch(w, row, 0,          ACS_LTEE);
    mvwhline(w, row, 1,          ACS_HLINE, LOBBY_W - 2);
    mvwaddch(w, row, LOBBY_W-1,  ACS_RTEE);
    if (label && label[0])
        mvwprintw(w, row, 2, " %s ", label);
}

static void lobby_draw_peers(WINDOW *w, Session *s) {
    char label[40];
    snprintf(label, sizeof(label), "connected  %d / %d", s->count, MAX_CLIENTS);
    lobby_section(w, 12, label);

    for (int r = PEER_ROW0; r < PEER_ROW0 + PEER_ROWS; r++) {
        wmove(w, r, 1); wclrtoeol(w);
    }

    if (s->count == 0) {
        mvwprintw(w, PEER_ROW0, 4, "(no peers yet)");
    } else {
        for (int i = 0; i < s->count && i < PEER_ROWS; i++) {
            char tbuf[8] = "--:--";
            if (g_join_times[i]) {
                struct tm *t = localtime(&g_join_times[i]);
                strftime(tbuf, sizeof(tbuf), "%H:%M", t);
            }
            mvwprintw(w, PEER_ROW0 + i, 4,
                      "%-24.24s  %s", s->nicks[i], tbuf);
        }
    }
    wrefresh(w);
}

static void lobby_draw_all(WINDOW *w, Session *s,
                            const char *password, const char *local_ip,
                            int port) {
    werase(w);
    box(w, 0, 0);

    mvwprintw(w, 2, (LOBBY_W - 9)  / 2, "term-chan");
    mvwprintw(w, 3, (LOBBY_W - 11) / 2, "create room");

    lobby_section(w, 5, NULL);

    mvwprintw(w, 7,  4, "host        %s", s->my_nick);
    if (password && password[0])
        mvwprintw(w, 8,  4, "password    %s   share with peers", password);
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
        int conn = accept_connection(listener_fd, peer_ip, peer_nick, peer_pass);
        if (conn < 0) continue;

        if (password && password[0] &&
            strcmp(peer_pass, password) != 0) {
            send_conn_wrong_pass(conn);
            continue;
        }
        if (s->count >= MAX_CLIENTS) {
            send_conn_reject(conn);
            continue;
        }
        if (!tui_accept_request(peer_nick, peer_ip)) {
            send_conn_reject(conn);
            lobby_draw_all(w, s, password, local_ip, port);
            continue;
        }

        send_conn_accept(conn, s->my_nick);
        room_add(s, conn, peer_nick);
        g_join_times[s->count - 1] = time(NULL);

        Packet rsync;
        memset(&rsync, 0, sizeof(rsync));
        rsync.type = ROSTER_SYNC;
        strncpy(rsync.sender, s->my_nick, MAX_NAME - 1);
        char *rp   = rsync.content;
        int   rrem = MAX_MSG - 1;
        int   n    = snprintf(rp, rrem, "%s\n", s->my_nick);
        if (n > 0 && n < rrem) { rp += n; rrem -= n; }
        for (int i = 0; i < s->count && rrem > 1; i++) {
            n = snprintf(rp, rrem, "%s\n", s->nicks[i]);
            if (n > 0 && n < rrem) { rp += n; rrem -= n; }
        }
        rsync.content[MAX_MSG - 1] = '\0';
        send(conn, &rsync, sizeof(Packet), 0);

        Packet join;
        memset(&join, 0, sizeof(join));
        join.type = PEER_JOIN;
        strncpy(join.sender, peer_nick, MAX_NAME - 1);
        snprintf(join.content, MAX_MSG - 1, "%s joined the room.", peer_nick);
        room_broadcast(s, &join, conn);

        lobby_draw_all(w, s, password, local_ip, port);
    }

    delwin(w);
    return 0;
}
