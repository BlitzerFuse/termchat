#include "chat.h"
#include "network.h"
#include "tui.h"
#include "protocol.h"
#include <pthread.h>
#include <unistd.h>
#include <ncurses.h>
#include <string.h>

#define DEFAULT_PORT 5000

int main(void) {
    MenuResult menu;
    if (tui_menu(&menu) < 0)
        return 0;

    int sock_fd;

    if (menu.mode == MODE_LISTEN) {
        int listener = init_listener(DEFAULT_PORT);
        if (listener < 0) {
            endwin();
            return 1;
        }

        while (1) {
            tui_waiting(DEFAULT_PORT, menu.password[0] ? menu.password : NULL);

            char peer_ip[64]         = {0};
            char peer_nick[MAX_NAME] = {0};
            char peer_pass[MAX_PASS] = {0};
            sock_fd = accept_connection(listener, peer_ip, peer_nick, peer_pass);
            if (sock_fd < 0) { endwin(); close(listener); return 1; }

            /* Check password if one is set */
            if (menu.password[0]) {
                if (strncmp(peer_pass, menu.password, MAX_PASS) != 0) {
                    send_conn_wrong_pass(sock_fd); /* closes sock_fd */
                    continue; /* back to waiting */
                }
            }

            if (tui_accept_request(peer_nick, peer_ip)) {
                send_conn_accept(sock_fd);
                break;
            } else {
                send_conn_reject(sock_fd);
            }
        }
        close(listener);

    } else {
        /* Prompt for password — connector always enters one;
           if the listener has no password set, an empty string is fine */
        const char *entered_pass = tui_enter_password(
            menu.peer_ip[0] ? menu.peer_ip : "peer", menu.peer_ip);

        sock_fd = connect_to_peer(menu.peer_ip, DEFAULT_PORT,
                                  menu.nickname, entered_pass);
        if (sock_fd == -3) {
            endwin();
            fprintf(stderr, "Wrong password.\n");
            return 1;
        }
        if (sock_fd == -2) {
            endwin();
            fprintf(stderr, "Connection was rejected by the peer.\n");
            return 1;
        }
        if (sock_fd < 0) { endwin(); return 1; }
    }

    tui_init(menu.nickname);
    start_chat(sock_fd, menu.nickname, tui_display_message);
    tui_shutdown();
    return 0;
}
