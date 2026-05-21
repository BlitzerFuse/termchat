#ifndef TUI_H
#define TUI_H

#include "protocol.h"
#include "session.h"
#include <stddef.h>

typedef enum { MODE_LISTEN, MODE_CONNECT } ConnMode;

typedef struct {
    ConnMode mode;
    char nickname[MAX_NAME];
    char peer_ip[64];
    char password[MAX_PASS];
    int  port;
    int  discovery_port;
} MenuResult;

int  tui_menu(MenuResult *out);
int  tui_accept_request(const char *peer_nick, const char *peer_ip);

/*
 * FIX M-5: signature changed from returning a pointer to a static buffer
 * to accepting a caller-owned buffer.  Eliminates the thread-unsafe static
 * and the silent overwrite that occurred in the retry loop in main.c.
 */
void tui_enter_password(const char *peer_nick, const char *peer_ip,
                        char *out, size_t out_len);

int  tui_lobby(Session *s, int listener_fd, const char *password, int port);

/* Waiting screen: blocks until CHAT_START received (0) or user quits (-1). */
int  tui_waiting_run(int sock, const char *host_nick,
                     const char *host_ip, int port,
                     const char *my_nick);

void tui_init(const char *nickname, Session *s);
void tui_shutdown(void);
void tui_handle_resize(void);
void tui_display_message(Packet *p);
void tui_status(const char *fmt, ...);
char *tui_get_input(void);
int  tui_was_resized(void);
void tui_clear_resize(void);

/*
 * FIX M-6: expose a setter so that /nick keeps the TUI input bar in sync
 * with the session nickname without the two copies diverging.
 */
void tui_set_nick(const char *new_nick);

#endif /* TUI_H */
