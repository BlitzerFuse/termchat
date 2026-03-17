#include "tui.h"
#include "tui_internal.h"
#include <ncurses.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static WINDOW *msg_win;
static WINDOW *input_win;
static char    prompt[MAX_NAME + 8];
static char    g_last_sender[MAX_NAME] = {0};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void draw_input_bar(void) {
    mvwhline(input_win, 0, 0, ACS_HLINE, COLS);
    mvwprintw(input_win, 1, 0, "%s", prompt);
    wclrtoeol(input_win);
    wrefresh(input_win);
}

/* ------------------------------------------------------------------ */
/* Internal API (tui_internal.h)                                       */
/* ------------------------------------------------------------------ */

void tui_clear_chat(void) {
    werase(msg_win);
    wrefresh(msg_win);
    draw_input_bar();
}

const char *tui_get_last_sender(void) {
    return g_last_sender[0] ? g_last_sender : NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void tui_init(const char *nickname) {
    ncurses_start();
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

    if (p->type == MSG) {
        /* Track last sender for /reply — only update on incoming messages
           (the chat loop calls display_cb for outgoing too, so we filter
           by checking whether sender matches the prompt's nick).
           We deliberately do NOT filter here — /reply replies to whoever
           spoke last, including yourself, which is harmless and simpler. */
        strncpy(g_last_sender, p->sender, MAX_NAME - 1);
        g_last_sender[MAX_NAME - 1] = '\0';
        wprintw(msg_win, "[%s] %s: %s\n", tbuf, p->sender, p->content);
    } else {
        /* Action message (/me) — content already formatted by sender */
        wprintw(msg_win, "[%s] %s\n", tbuf, p->content);
    }

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
    if (tui_was_resized()) return NULL;
    draw_input_bar();
    wmove(input_win, 1, (int)strlen(prompt));
    echo();
    char buf[MAX_MSG];
    int r = wgetnstr(input_win, buf, sizeof(buf) - 1);
    noecho();
    if (r == ERR || tui_was_resized()) return NULL;
    return strdup(buf);
}
