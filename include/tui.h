#ifndef TUI_H
#define TUI_H

#include "protocol.h"

typedef enum { MODE_LISTEN, MODE_CONNECT } ConnMode;

typedef struct {
    ConnMode mode;
    char nickname[MAX_NAME];
    char peer_ip[64];
    char password[MAX_PASS];
} MenuResult;

int tui_menu(MenuResult *out);
int tui_accept_request(const char *peer_nick, const char *peer_ip);
void tui_waiting(int port, const char *password);
const char *tui_enter_password(const char *peer_nick, const char *peer_ip);
void tui_init(const char *nickname);
void tui_shutdown(void);
void tui_display_message(Packet *p);
void tui_status(const char *fmt, ...);
char *tui_get_input(void);
int tui_was_resized(void);

#endif
