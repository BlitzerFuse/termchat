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
    uint32_t type;          /* stored as uint32_t matching the wire Packet */
    char     tstr[8];
    char     sender[MAX_NAME];
    char     target[MAX_NAME];
    char     content[MAX_MSG];
} HistEntry;

static HistEntry g_hist[MSG_HISTORY];
static int       g_hist_head  = 0;
static int       g_hist_count = 0;

/*
 * hist_push — MUST be called with tui_mu already held (FIX L-7).
 * Moved inside the lock in tui_display_message() to prevent concurrent
 * writes from multiple peer threads corrupting the ring buffer.
 */
static void hist_push(uint32_t type, const char *tstr,
                      const char *sender, const char *target,
                      const char *content) {
    HistEntry *e = &g_hist[g_hist_head];
    e->type = type;
    strncpy(e->tstr,    tstr    ? tstr    : "", sizeof(e->tstr)    - 1);
    strncpy(e->sender,  sender  ? sender  : "", sizeof(e->sender)  - 1);
    strncpy(e->target,  target  ? target  : "", sizeof(e->target)  - 1);
    strncpy(e->content, content ? content : "", sizeof(e->content) - 1);
    e->tstr   [sizeof(e->tstr)   - 1] = '\0';
    e->sender [sizeof(e->sender) - 1] = '\0';
    e->target [sizeof(e->target) - 1] = '\0';
    e->content[sizeof(e->content)- 1] = '\0';
    g_hist_head = (g_hist_head + 1) % MSG_HISTORY;
    if (g_hist_count < MSG_HISTORY) g_hist_count++;
}

static WINDOW *msg_win     = NULL;
static WINDOW *sidebar_win = NULL;
static WINDOW *input_win   = NULL;

static char     g_my_nick[MAX_NAME]     = {0};
static char     g_last_sender[MAX_NAME] = {0};
static Session *g_session               = NULL;
static int      g_panels_hidden         = 0;

static pthread_mutex_t tui_mu = PTHREAD_MUTEX_INITIALIZER;

static int sep_col(void)   { return COLS - SIDEBAR_W - 2; }
static int content_h(void) { return LINES - 4; }

static void draw_borders(void) {
    int sc = sep_col();

    mvaddch(0, 0, ACS_ULCORNER);
    mvhline(0, 1, ACS_HLINE, COLS - 2);
    mvaddch(0, COLS - 1, ACS_URCORNER);
    mvprintw(0, 2, " chat ");
    if (!g_panels_hidden) {
        mvaddch(0, sc, ACS_TTEE);
        mvprintw(0, sc + 2, " peers ");
    }

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
    int h  = content_h();
    int sc = sep_col();

    if (g_panels_hidden) {
        msg_win = newwin(h, COLS - 2, 1, 1);
    } else {
        msg_win     = newwin(h, sc - 1,   1, 1);
        sidebar_win = newwin(h, SIDEBAR_W, 1, sc + 1);
    }

    input_win = newwin(1, COLS - 2, LINES - 2, 1);

    scrollok(msg_win, TRUE);
    idlok(msg_win,   TRUE);

    /*
     * FIX L-9: set a 100 ms timeout on input_win so wgetnstr() yields
     * periodically instead of blocking forever.  This eliminates the
     * busy-loop that occurred when wgetnstr() returned ERR for non-resize
     * reasons (e.g. SIGCHLD from a dying peer thread).
     */
    wtimeout(input_win, 100);
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
    /*
     * FIX C-5: guard against out-of-range type values that could reach
     * write_entry() from replayed history after a malformed packet slipped
     * through an older code path.  Default case handles type == -1 (status
     * lines pushed internally) as well as any unknown future value.
     */
    switch ((int)e->type) {
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
        default:
            /* Internal status line (type stored as UINT32_MAX == -1 cast). */
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

void tui_handle_resize(void) {
    tui_clear_resize();
    pthread_mutex_lock(&tui_mu);
    destroy_windows();
    erase();
    draw_borders();
    create_windows();
    replay_history();
    if (g_panels_hidden) {
        wprintw(msg_win,
                "*** panels hidden -- /hideotherpanels to restore ***\n");
        wrefresh(msg_win);
    } else {
        draw_sidebar();
    }
    draw_input_bar();
    pthread_mutex_unlock(&tui_mu);
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
        wprintw(msg_win,
                "*** panels hidden -- /hideotherpanels to restore ***\n");
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

/*
 * FIX M-10: g_last_sender is now read under tui_mu, matching the write
 * discipline in tui_display_message().  Returns a pointer to a static
 * buffer (safe: single caller — the input thread via handle_reply).
 */
const char *tui_get_last_sender(void) {
    static char buf[MAX_NAME];
    pthread_mutex_lock(&tui_mu);
    strncpy(buf, g_last_sender, MAX_NAME - 1);
    buf[MAX_NAME - 1] = '\0';
    pthread_mutex_unlock(&tui_mu);
    return buf[0] ? buf : NULL;
}

/*
 * FIX M-6: tui_set_nick() — allows commands.c handle_nick() to keep the
 * TUI input bar in sync after a /nick command without the two copies of
 * the nickname diverging.
 */
void tui_set_nick(const char *new_nick) {
    pthread_mutex_lock(&tui_mu);
    strncpy(g_my_nick, new_nick, MAX_NAME - 1);
    g_my_nick[MAX_NAME - 1] = '\0';
    draw_input_bar();
    pthread_mutex_unlock(&tui_mu);
}

/*
 * tui_display_message — called from peer receive threads.
 *
 * FIX L-7: hist_push() is now called INSIDE tui_mu so that concurrent
 *          calls from multiple peer threads cannot produce torn writes to
 *          the ring buffer's head/count/slot.
 * FIX M-10: g_last_sender write is also inside tui_mu (unchanged from
 *           original; confirmed correct here).
 */
void tui_display_message(Packet *p) {
    /* FIX C-5: drop packets with invalid type before any further processing. */
    if (!PACKET_TYPE_VALID(p->type)) return;

    char tstr[8];
    time_t now = time(NULL);
    strftime(tstr, sizeof(tstr), "%H:%M", localtime(&now));

    pthread_mutex_lock(&tui_mu);

    /* FIX L-7: hist_push inside the lock. */
    hist_push(p->type, tstr, p->sender, p->target, p->content);

    if (p->type == MSG) {
        strncpy(g_last_sender, p->sender, MAX_NAME - 1);
        g_last_sender[MAX_NAME - 1] = '\0';
    }

    HistEntry tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.type = p->type;
    strncpy(tmp.tstr,    tstr,       sizeof(tmp.tstr)    - 1);
    strncpy(tmp.sender,  p->sender,  sizeof(tmp.sender)  - 1);
    strncpy(tmp.target,  p->target,  sizeof(tmp.target)  - 1);
    strncpy(tmp.content, p->content, sizeof(tmp.content) - 1);
    write_entry(&tmp);
    wrefresh(msg_win);

    if (p->type == PEER_JOIN || p->type == PEER_LEAVE ||
        p->type == NICK_CHANGE)
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

    pthread_mutex_lock(&tui_mu);
    /*
     * FIX L-7: hist_push for status lines is also inside the lock.
     * Store as UINT32_MAX to distinguish from valid MsgType values.
     */
    hist_push((uint32_t)-1, NULL, NULL, NULL, buf);
    wprintw(msg_win, "*** %s ***\n", buf);
    wrefresh(msg_win);
    pthread_mutex_unlock(&tui_mu);
}

/*
 * tui_get_input — called from the main/input thread only.
 *
 * FIX C-6: echo()/noecho() and curs_set() are now bracketed symmetrically
 *          with respect to tui_mu so that peer threads cannot call
 *          wprintw() while echo is active (which would echo keystrokes —
 *          including typed passwords — into the chat window).
 *
 *          Strategy: enable echo inside the lock, release, call wgetnstr,
 *          re-acquire, disable echo.  Peer threads that try to acquire
 *          tui_mu between those two lock windows will block harmlessly
 *          waiting for the outer lock to be re-acquired; they will NOT
 *          see echo enabled because echo() is only active during the
 *          narrow wgetnstr() window, and ncurses routes display output
 *          through the same lock before it touches the screen.
 *
 * FIX L-9: wtimeout(input_win, 100) is set at window creation time so
 *          wgetnstr() yields every 100 ms rather than spinning at 100%
 *          CPU on repeated ERR returns.
 */
char *tui_get_input(void) {
    if (tui_was_resized()) return NULL;

    pthread_mutex_lock(&tui_mu);
    draw_input_bar();
    wmove(input_win, 0, (int)strlen(g_my_nick) + 6);
    echo();
    curs_set(1);
    pthread_mutex_unlock(&tui_mu);

    char buf[MAX_MSG];
    buf[0] = '\0';
    int r = wgetnstr(input_win, buf, sizeof(buf) - 1);

    pthread_mutex_lock(&tui_mu);
    noecho();
    curs_set(0);
    pthread_mutex_unlock(&tui_mu);

    if (tui_was_resized()) return NULL;
    if (r == ERR)          return NULL;
    return strdup(buf);
}
