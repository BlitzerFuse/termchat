#ifndef TUI_H
#define TUI_H

#include "protocol.h"

typedef enum { MODE_LISTEN, MODE_CONNECT } ConnMode;

typedef struct {
    char     nickname[MAX_NAME];
    ConnMode mode;
    char     peer_ip[64];
} MenuResult;

/* Startup menu. Leaves ncurses running on success (returns 0).
   Returns -1 on abort (ncurses shut down). */
int   tui_menu(MenuResult *out);

/* Show accept/reject screen when a peer requests to connect.
   Returns 1 if accepted, 0 if rejected. */
int   tui_accept_request(const char *peer_nick, const char *peer_ip);

/* Show a "waiting for connection" screen. Ncurses must already be running. */
void  tui_waiting(int port);

/* Init the chat TUI. Safe to call with ncurses already running. */
void  tui_init(const char *nickname);

void  tui_shutdown(void);
void  tui_display_message(Packet *p);
void  tui_status(const char *fmt, ...);
char *tui_get_input(void);
int   tui_was_resized(void);

#endif
