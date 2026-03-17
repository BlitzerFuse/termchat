#ifndef TUI_INTERNAL_H
#define TUI_INTERNAL_H
#include <stddef.h>

void ncurses_start(void);

void tui_clear_chat(void);

const char *tui_get_last_sender(void);

void tui_get_local_ip(char *buf, size_t len);

#endif
