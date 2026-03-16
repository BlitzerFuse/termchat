#include "chat.h"
#include "network.h"
#include "tui.h"
#include "protocol.h"
#include <pthread.h>
#include <unistd.h>
#include <ncurses.h>

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

        /* Keep accepting and potentially rejecting until someone is accepted */
        while (1) {
            tui_waiting(DEFAULT_PORT);

            char peer_ip[64]   = {0};
            char peer_nick[MAX_NAME] = {0};
            sock_fd = accept_connection(listener, peer_ip, peer_nick);
            if (sock_fd < 0) { endwin(); close(listener); return 1; }

            if (tui_accept_request(peer_nick, peer_ip)) {
                send_conn_accept(sock_fd);
                break;  /* accepted — proceed to chat */
            } else {
                send_conn_reject(sock_fd);  /* closes sock_fd internally */
                /* loop back and wait for next connection */
            }
        }
        close(listener);

    } else {
        sock_fd = connect_to_peer(menu.peer_ip, DEFAULT_PORT, menu.nickname);
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
