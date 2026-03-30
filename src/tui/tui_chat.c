#include "tui.h"
#include "tui_internal.h"
#include <ncurses.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define SIDEBAR_W   16
#define MSG_HISTORY 500

typedef struct {
    int  type;
    char tstr[8];
    char sender[MAX_NAME];
    char target[MAX_NAME];
    char content[MAX_MSG];
} HistEntry;

static HistEntry g_hist[MSG_HISTORY];
static int       g_hist_head  = 0;
static int       g_hist_count = 0;

static void hist_push(int type, const char *tstr,
                      const char *sender, const char *target,
                      const char *content) {
    HistEntry *e = &g_hist[g_hist_head];
    e->type = type;
    strncpy(e->tstr,    tstr    ? tstr    : "", sizeof(e->tstr)    - 1);
    strncpy(e->sender,  sender  ? sender  : "", sizeof(e->sender)  - 1);
    strncpy(e->target,  target  ? target  : "", sizeof(e->target)  - 1);
    strncpy(e->content, content ? content : "", sizeof(e->content) - 1);
    e->tstr[sizeof(e->tstr)-1]       = '\0';
    e->sender[sizeof(e->sender)-1]   = '\0';
    e->target[sizeof(e->target)-1]   = '\0';
    e->content[sizeof(e->content)-1] = '\0';
    g_hist_head = (g_hist_head + 1) % MSG_HISTORY;
    if (g_hist_count < MSG_HISTORY) g_hist_count++;
}

static WINDOW *msg_win     = NULL;
static WINDOW *sidebar_win = NULL;
static WINDOW *input_win   = NULL;

static char     g_my_nick[MAX_NAME]    = {0};
static char     g_last_sender[MAX_NAME]= {0};
static Session *g_session              = NULL;
static int      g_panels_hidden        = 0;

static pthread_mutex_t tui_mu = PTHREAD_MUTEX_INITIALIZER;
static int sep_col(void)    { return COLS - SIDEBAR_W - 2; }
static int msg_inner_w(void){ return g_panels_hidden ? COLS - 2 : sep_col() - 1; }
static int content_h(void)  { return LINES - 4; } /* rows 1 .. LINES-4 */

static void draw_borders(void) {
    int sc = sep_col();

    mvaddch(0, 0, ACS_ULCORNER);
    mvhline(0, 1, ACS_HLINE, COLS - 2);
    mvaddch(0, COLS - 1, ACS_URCORNER);
    mvprintw(0, 2, " chat ");
    if (!g_panels_hidden) {
        mvaddch(0, sc, ACS_TTEE);
        mvprintw(0, sc + 2, " peers ");    }

    for (int r = 1; r <= LINES - 4; r++) {
        mvaddch(r, 0,        ACS_VLINE);
        mvaddch(r, COLS - 1, ACS_VLINE);
        if (!g_panels_hidden)
            mvaddch(r, sc, ACS_VLINE);
    }

    mvaddch(LINES - 3, 0, ACS_LTEE);
    mvhline(LINES - 3, 1, ACS_HLINE, COLS - 2);
    if (!g_panels_hidden)
        mvaddch(LINES - 3, sc, ACS_BTEE);
    mvaddch(LINES - 3, COLS - 1, ACS_RTEE);

    mvaddch(LINES - 2, 0,        ACS_VLINE);
    mvaddch(LINES - 2, COLS - 1, ACS_VLINE);

    mvaddch(LINES - 1, 0, ACS_LLCORNER);
    mvhline(LINES - 1, 1, ACS_HLINE, COLS - 2);
    mvaddch(LINES - 1, COLS - 1, ACS_LRCORNER);

    refresh();
}

static void destroy_windows(void) {
    if (msg_win)     { delwin(msg_win);     msg_win     = NULL; }
    if (sidebar_win) { delwin(sidebar_win); sidebar_win = NULL; }
    if (input_win)   { delwin(input_win);   input_win   = NULL; }
}

static void create_windows(void) {
    int h  = content_h();   /* rows 1 .. LINES-4 */
    int sc = sep_col();

    if (g_panels_hidden) {
        msg_win = newwin(h, COLS - 2, 1, 1);
    } else {
        msg_win     = newwin(h, sc - 1,  1, 1);
        sidebar_win = newwin(h, SIDEBAR_W, 1, sc + 1);
    }

    input_win = newwin(1, COLS - 2, LINES - 2, 1);

    scrollok(msg_win, TRUE);
    idlok(msg_win,   TRUE);
}

static void draw_sidebar(void) {
    if (g_panels_hidden || !sidebar_win) return;

    werase(sidebar_win);

    char you[MAX_NAME + 8];
    snprintf(you, sizeof(you), "%s (you)", g_my_nick);
    mvwprintw(sidebar_win, 0, 0, " %-*.*s",
              SIDEBAR_W - 1, SIDEBAR_W - 1, you);

    if (!g_session) { wrefresh(sidebar_win); return; }

    int row = 1;
    for (int i = 0; i < g_session->count && row < content_h(); i++, row++)
        mvwprintw(sidebar_win, row, 0, " %-*.*s",
                  SIDEBAR_W - 1, SIDEBAR_W - 1, g_session->nicks[i]);

    wrefresh(sidebar_win);
}

static void write_entry(HistEntry *e) {
    switch (e->type) {
        case MSG:
            if (e->target[0])
                wprintw(msg_win, "[%s] [%s -> %s]: %s\n",
                        e->tstr, e->sender, e->target, e->content);
            else
                wprintw(msg_win, "[%s] %s: %s\n",
                        e->tstr, e->sender, e->content);
            break;
        case PEER_JOIN:
        case PEER_LEAVE:
            wprintw(msg_win, "--- %s ---\n", e->content);
            break;
        case NICK_CHANGE:
            wprintw(msg_win, "--- %s is now known as %s ---\n",
                    e->sender, e->content);
            break;
        default:                               /* status line (type == -1) */
            wprintw(msg_win, "*** %s ***\n", e->content);
            break;
    }
}

static void replay_history(void) {
    werase(msg_win);
    int start = (g_hist_count < MSG_HISTORY) ? 0 : g_hist_head;
    for (int n = 0; n < g_hist_count; n++)
        write_entry(&g_hist[(start + n) % MSG_HISTORY]);
    wrefresh(msg_win);
}

static void draw_input_bar(void) {
    werase(input_win);
    wprintw(input_win, " [%s] > ", g_my_nick);
    wrefresh(input_win);
}

void tui_init(const char *nickname, Session *s) {
    g_session = s;
    strncpy(g_my_nick, nickname, MAX_NAME - 1);
    g_my_nick[MAX_NAME - 1] = '\0';

    ncurses_start();
    keypad(stdscr, TRUE);

    erase();
    draw_borders();
    create_windows();
    draw_sidebar();
    draw_input_bar();
}

void tui_shutdown(void) {
    destroy_windows();
    endwin();
}

void tui_toggle_panels(void) {
    pthread_mutex_lock(&tui_mu);
    g_panels_hidden = !g_panels_hidden;
    destroy_windows();
    erase();
    draw_borders();
    create_windows();
    replay_history();
    if (g_panels_hidden) {
        wprintw(msg_win, "*** panels hidden -- /hideotherpanels to restore ***\n");
        wrefresh(msg_win);
    } else {
        draw_sidebar();
    }
    draw_input_bar();
    pthread_mutex_unlock(&tui_mu);
}

void tui_clear_chat(void) {
    pthread_mutex_lock(&tui_mu);
    werase(msg_win);
    wrefresh(msg_win);
    draw_input_bar();
    pthread_mutex_unlock(&tui_mu);
}

const char *tui_get_last_sender(void) {
    return g_last_sender[0] ? g_last_sender : NULL;
}

void tui_display_message(Packet *p) {
    char tstr[8];
    time_t now = time(NULL);
    strftime(tstr, sizeof(tstr), "%H:%M", localtime(&now));

    hist_push(p->type, tstr, p->sender, p->target, p->content);

    pthread_mutex_lock(&tui_mu);

    if (p->type == MSG) {
        strncpy(g_last_sender, p->sender, MAX_NAME - 1);
        g_last_sender[MAX_NAME - 1] = '\0';
    }

    HistEntry tmp;
    strncpy(tmp.tstr,    tstr,      sizeof(tmp.tstr)    - 1);
    strncpy(tmp.sender,  p->sender, sizeof(tmp.sender)  - 1);
    strncpy(tmp.target,  p->target, sizeof(tmp.target)  - 1);
    strncpy(tmp.content, p->content,sizeof(tmp.content) - 1);
    tmp.tstr[sizeof(tmp.tstr)-1]       = '\0';
    tmp.sender[sizeof(tmp.sender)-1]   = '\0';
    tmp.target[sizeof(tmp.target)-1]   = '\0';
    tmp.content[sizeof(tmp.content)-1] = '\0';
    tmp.type = p->type;
    write_entry(&tmp);
    wrefresh(msg_win);

    if (p->type == PEER_JOIN || p->type == PEER_LEAVE || p->type == NICK_CHANGE)
        draw_sidebar();

    wrefresh(input_win);
    pthread_mutex_unlock(&tui_mu);
}

void tui_status(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    hist_push(-1, NULL, NULL, NULL, buf);

    pthread_mutex_lock(&tui_mu);
    wprintw(msg_win, "*** %s ***\n", buf);
    wrefresh(msg_win);
    pthread_mutex_unlock(&tui_mu);
}

char *tui_get_input(void) {
    if (tui_was_resized()) return NULL;

    pthread_mutex_lock(&tui_mu);
    draw_input_bar();
    wmove(input_win, 0, (int)strlen(g_my_nick) + 6);
    echo();
    pthread_mutex_unlock(&tui_mu);

    char buf[MAX_MSG];
    int  r = wgetnstr(input_win, buf, sizeof(buf) - 1);

    pthread_mutex_lock(&tui_mu);
    noecho();
    pthread_mutex_unlock(&tui_mu);

    if (r == ERR || tui_was_resized()) return NULL;
    return strdup(buf);
}
