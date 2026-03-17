#include "tui.h"
#include "tui_internal.h"
#include <ncurses.h>
#include <signal.h>

static volatile sig_atomic_t g_resized = 0;

static void handle_sigwinch(int sig) {
    (void)sig;
    g_resized = 1;
}

int tui_was_resized(void) {
    return g_resized;
}

void ncurses_start(void) {
    if (stdscr) return;
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    signal(SIGWINCH, handle_sigwinch);
}
