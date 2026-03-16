#include "tui.h"
#include "discovery.h"
#include <ncurses.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

static WINDOW *msg_win;
static WINDOW *input_win;
static char    prompt[MAX_NAME + 8];

/* Set by SIGWINCH; checked in tui_get_input() to break the chat loop. */
static volatile sig_atomic_t g_resized = 0;

static void handle_sigwinch(int sig) {
    (void)sig;
    g_resized = 1;
}

int tui_was_resized(void) {
    return g_resized;
}

/* Thread entry point: run discovery_respond with a nickname string. */
static void *tui_menu_disc_thread(void *arg) {
    discovery_respond((const char *)arg);
    return NULL;
}

/* Scan thread: runs discover_peers in background so ncurses stays alive. */
typedef struct {
    Peer        peers[MAX_PEERS];
    int         count;
    const char *nickname;
    int         done;  /* set to 1 when finished */
} ScanArgs;

static void *scan_thread(void *arg) {
    ScanArgs *s = (ScanArgs *)arg;
    s->count = discover_peers(s->peers, MAX_PEERS, s->nickname);
    s->done  = 1;
    return NULL;
}

/* ── helpers ─────────────────────────────────────────────── */

static void get_local_ip(char *buf, size_t len) {
    strncpy(buf, "unavailable", len - 1);
    buf[len - 1] = '\0';
    FILE *f = popen("hostname -i 2>/dev/null", "r");
    if (!f) return;
    char tmp[128] = {0};
    if (fgets(tmp, sizeof(tmp), f)) {
        char *tok = strtok(tmp, " \t\n");
        if (tok) { strncpy(buf, tok, len - 1); buf[len - 1] = '\0'; }
    }
    pclose(f);
}

static void ncurses_start(void) {
    if (stdscr) return;   /* already running */
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    signal(SIGWINCH, handle_sigwinch);
}

/* ── startup menu ────────────────────────────────────────── */

/* Draw the discover peer list inside the menu box.
   selected is the highlighted index (-1 = none). */
static void draw_peer_list(WINDOW *w, Peer *peers, int count,
                            int sel, int scanning) {
    /* clear the peer area (rows 10-) */
    for (int r = 10; r < 18; r++) {
        wmove(w, r, 1);
        wclrtoeol(w);
    }

    if (scanning) {
        mvwprintw(w, 10, 2, "Scanning...");
        return;
    }
    if (count == 0) {
        mvwprintw(w, 10, 2, "(no peers found - type IP manually below)");
        return;
    }
    mvwprintw(w, 10, 2, "Peers found (Enter to select):");
    for (int i = 0; i < count && i < 6; i++) {
        if (i == sel) wattron(w, A_REVERSE);
        mvwprintw(w, 11 + i, 3, " %-20s %s ", peers[i].nickname, peers[i].ip);
        if (i == sel) wattroff(w, A_REVERSE);
    }
}

int tui_menu(MenuResult *out) {
    ncurses_start();

    char local_ip[64];
    get_local_ip(local_ip, sizeof(local_ip));

    /* Box is taller now to fit peer list: 22 rows */
    const int bw = 56, bh = 22;
    int bx = (COLS - bw) / 2, by = (LINES - bh) / 2;
    if (by < 0) by = 0;
    if (bx < 0) bx = 0;

    WINDOW *w = newwin(bh, bw, by, bx);
    keypad(w, TRUE);

    /* draw static chrome */
    werase(w);
    box(w, 0, 0);
    mvwprintw(w, 1, (bw - 8) / 2, "termchat");
    mvwhline(w, 2, 1, ACS_HLINE, bw - 2);
    mvwprintw(w, 3, 2, "Your IP : %s", local_ip);
    mvwprintw(w, 4, 2, "Port    : 5000");
    mvwhline(w, 5, 1, ACS_HLINE, bw - 2);
    mvwprintw(w, 6, 2, "Nickname: ");
    wrefresh(w);

    /* nickname */
    echo(); curs_set(1);
    wmove(w, 6, 12);
    wgetnstr(w, out->nickname, MAX_NAME - 1);
    noecho(); curs_set(0);
    if (!out->nickname[0]) goto abort;

    /* mode selection */
    mvwhline(w, 7, 1, ACS_HLINE, bw - 2);
    mvwprintw(w, 8, 2, "Mode (arrows + Enter):");

    int mode_sel = 0;
    while (1) {
        if (mode_sel == 0) wattron(w, A_REVERSE);
        mvwprintw(w, 9, 4, " Listen (wait) ");
        if (mode_sel == 0) wattroff(w, A_REVERSE);
        if (mode_sel == 1) wattron(w, A_REVERSE);
        mvwprintw(w, 9, 21, " Connect       ");
        if (mode_sel == 1) wattroff(w, A_REVERSE);
        wrefresh(w);

        int ch = wgetch(w);
        switch (ch) {
            case KEY_LEFT:  case KEY_UP:    mode_sel = 0; break;
            case KEY_RIGHT: case KEY_DOWN:  mode_sel = 1; break;
            case '\t':                      mode_sel ^= 1; break;
            case '\n': case '\r':           goto mode_done;
            case KEY_RESIZE:                goto abort;
            case 'q':  case 27:             goto abort;
        }
    }
mode_done:
    out->mode = (mode_sel == 0) ? MODE_LISTEN : MODE_CONNECT;

    if (out->mode == MODE_LISTEN) {
        /* Start the discovery responder immediately — before we block on
           accept_connection — so any peer that scans while we're on the
           waiting screen gets a reply.  The thread is detached; it will
           exit on its own once the TCP connection is established and
           main() closes the process, or on any recvfrom error. */
        pthread_t disc_tid;
        pthread_create(&disc_tid, NULL,
                       tui_menu_disc_thread, out->nickname);
        pthread_detach(disc_tid);
    }

    if (out->mode == MODE_CONNECT) {
        mvwhline(w, 10, 1, ACS_HLINE, bw - 2);

        /* run scan in a thread so ncurses stays responsive */
        ScanArgs scan = { .nickname = out->nickname, .done = 0, .count = 0 };
        pthread_t scan_tid;
        pthread_create(&scan_tid, NULL, scan_thread, &scan);

        /* animated spinner while waiting */
        const char *frames[] = { "Scanning   ", "Scanning.  ", "Scanning.. ", "Scanning..." };
        int frame = 0;
        wtimeout(w, 120); /* non-blocking wgetch every ~120ms */
        while (!scan.done) {
            mvwprintw(w, 10, 2, "%s", frames[frame++ % 4]);
            wrefresh(w);
            wgetch(w); /* drives the event loop; returns ERR on timeout */
        }
        wtimeout(w, -1); /* restore blocking mode */
        pthread_join(scan_tid, NULL);

        draw_peer_list(w, scan.peers, scan.count, 0, 0);

        /* separator: "--- or type IP manually ---" spanning full inner width */
        mvwhline(w, 18, 1, ACS_HLINE, bw - 2);
        const char *label = " or type IP manually ";
        int label_col = (bw - (int)strlen(label)) / 2;
        mvwprintw(w, 18, label_col, "%s", label);
        mvwprintw(w, 19, 2, "Peer IP : ");
        wrefresh(w);

        int peer_sel = 0;
        /* if peers found, let user navigate list first */
        while (scan.count > 0) {
            draw_peer_list(w, scan.peers, scan.count, peer_sel, 0);
            wrefresh(w);
            int ch = wgetch(w);
            switch (ch) {
                case KEY_UP:
                    if (peer_sel > 0) peer_sel--;
                    break;
                case KEY_DOWN:
                    if (peer_sel < scan.count - 1) peer_sel++;
                    break;
                case '\n': case '\r':
                    strncpy(out->peer_ip, scan.peers[peer_sel].ip,
                            sizeof(out->peer_ip) - 1);
                    goto connect_done;
                case '\t': case 'i':
                    goto manual_ip;
                case KEY_RESIZE:
                case 'q': case 27:
                    goto abort;
            }
        }

manual_ip:
        /* manual IP entry */
        wmove(w, 19, 12);
        echo(); curs_set(1);
        wgetnstr(w, out->peer_ip, (int)sizeof(out->peer_ip) - 1);
        noecho(); curs_set(0);
        if (!out->peer_ip[0]) goto abort;
    }

connect_done:
    delwin(w);
    /* intentionally do NOT call endwin() — leave ncurses running */
    return 0;

abort:
    delwin(w);
    endwin();
    return -1;
}

/* ── accept / reject screen ─────────────────────────────── */

int tui_accept_request(const char *peer_nick, const char *peer_ip) {
    clear();
    refresh();

    const int bw = 50, bh = 10;
    int bx = (COLS  - bw) / 2;
    int by = (LINES - bh) / 2;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;

    WINDOW *w = newwin(bh, bw, by, bx);
    keypad(w, TRUE);

    int sel = 1; /* default to Accept */

    while (1) {
        werase(w);
        box(w, 0, 0);
        mvwprintw(w, 1, (bw - 20) / 2, "Incoming Connection");
        mvwhline(w, 2, 1, ACS_HLINE, bw - 2);
        mvwprintw(w, 3, 2, "User : %s", peer_nick);
        mvwprintw(w, 4, 2, "IP   : %s", peer_ip);
        mvwhline(w, 5, 1, ACS_HLINE, bw - 2);
        mvwprintw(w, 6, 2, "Accept this connection?");

        if (sel == 0) wattron(w, A_REVERSE);
        mvwprintw(w, 7, 8,  " Reject ");
        if (sel == 0) wattroff(w, A_REVERSE);

        if (sel == 1) wattron(w, A_REVERSE);
        mvwprintw(w, 7, 32, " Accept ");
        if (sel == 1) wattroff(w, A_REVERSE);

        wrefresh(w);

        int ch = wgetch(w);
        switch (ch) {
            case KEY_LEFT:  case KEY_UP:    sel = 0; break;
            case KEY_RIGHT: case KEY_DOWN:  sel = 1; break;
            case '\t':                      sel ^= 1; break;
            case '\n': case '\r':           goto done;
            case 'y': case 'Y':             sel = 1; goto done;
            case 'n': case 'N':             sel = 0; goto done;
        }
    }
done:
    delwin(w);
    clear();
    refresh();
    return sel; /* 1 = accept, 0 = reject */
}

/* ── waiting screen ─────────────────────────────────────── */

void tui_waiting(int port) {
    clear();
    int cy = LINES / 2;
    int cx = (COLS - 40) / 2;
    if (cx < 0) cx = 0;

    /* Use ncurses ACS line-drawing so box chars render correctly on all
       terminals regardless of locale/encoding — raw UTF-8 in mvprintw
       produces garbled output like ~T~@ on many setups. */
    const int w = 38; /* interior width */

    /* top border */
    mvaddch(cy - 1, cx,         ACS_ULCORNER);
    mvhline(cy - 1, cx + 1,     ACS_HLINE, w);
    mvaddch(cy - 1, cx + w + 1, ACS_URCORNER);

    /* row 1: port */
    mvaddch(cy, cx,         ACS_VLINE);
    char line1[40];
    snprintf(line1, sizeof(line1), "  Waiting for connection on port %-4d", port);
    mvprintw(cy, cx + 1, "%-*s", w, line1);
    mvaddch(cy, cx + w + 1, ACS_VLINE);

    /* row 2: cancel hint */
    mvaddch(cy + 1, cx,         ACS_VLINE);
    mvprintw(cy + 1, cx + 1, "%-*s", w, "  Press Ctrl-C to cancel");
    mvaddch(cy + 1, cx + w + 1, ACS_VLINE);

    /* bottom border */
    mvaddch(cy + 2, cx,         ACS_LLCORNER);
    mvhline(cy + 2, cx + 1,     ACS_HLINE, w);
    mvaddch(cy + 2, cx + w + 1, ACS_LRCORNER);

    refresh();
}

/* ── chat TUI ────────────────────────────────────────────── */

static void draw_input_bar(void) {
    mvwhline(input_win, 0, 0, ACS_HLINE, COLS);
    mvwprintw(input_win, 1, 0, "%s", prompt);
    wclrtoeol(input_win);
    wrefresh(input_win);
}

void tui_init(const char *nickname) {
    ncurses_start();   /* no-op if already running */
    keypad(stdscr, TRUE);

    int msg_h = LINES - 2;
    msg_win   = newwin(msg_h, COLS, 0,     0);
    input_win = newwin(2,     COLS, msg_h, 0);

    scrollok(msg_win, TRUE);
    idlok(msg_win, TRUE);

    snprintf(prompt, sizeof(prompt), "[%s] > ", nickname);

    wprintw(msg_win, "termchat - connected as %s\n"
                     "Type /help for commands.\n\n", nickname);
    wrefresh(msg_win);
    draw_input_bar();
}

void tui_shutdown(void) {
    delwin(msg_win);
    delwin(input_win);
    endwin();
}

void tui_display_message(Packet *p) {
    char tbuf[6];
    time_t now = time(NULL);
    strftime(tbuf, sizeof(tbuf), "%H:%M", localtime(&now));
    wprintw(msg_win, "[%s] %s: %s\n", tbuf, p->sender, p->content);
    wrefresh(msg_win);
    wrefresh(input_win);
}

void tui_status(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    wprintw(msg_win, "*** %s ***\n", buf);
    wrefresh(msg_win);
}

char *tui_get_input(void) {
    if (g_resized) return NULL;
    draw_input_bar();
    wmove(input_win, 1, (int)strlen(prompt));
    echo();
    char buf[MAX_MSG];
    int r = wgetnstr(input_win, buf, sizeof(buf) - 1);
    noecho();
    if (r == ERR || g_resized) return NULL;
    return strdup(buf);
}
