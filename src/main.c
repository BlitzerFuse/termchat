#include "chat.h"
#include "network.h"
#include "session.h"
#include "room.h"
#include "tui.h"
#include "discovery.h"
#include "config.h"
#include "firewall.h"
#include "protocol.h"
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-p <port>]\n", prog);
    fprintf(stderr, "  -p, --port <port>  TCP port (default: from config or 5000)\n");
}

int main(int argc, char *argv[]) {
    Config cfg;
    config_defaults(&cfg);
    config_load(&cfg);

    int cli_port = 0;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0)
            && i + 1 < argc) {
            cli_port = atoi(argv[++i]);
            if (cli_port <= 0 || cli_port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    MenuResult menu = {0};
    strncpy(menu.nickname, cfg.nickname, MAX_NAME - 1);
    menu.port           = cli_port ? cli_port : cfg.port;
    menu.discovery_port = cfg.discovery_port;

    if (tui_menu(&menu) < 0)
        return 0;

    strncpy(cfg.nickname, menu.nickname, MAX_NAME - 1);
    cfg.port = menu.port;
    config_save(&cfg);

    int port      = menu.port;
    int disc_port = menu.discovery_port;

    Session s = {0};
    s.listener_fd = -1;
    strncpy(s.my_nick,  menu.nickname, MAX_NAME - 1);
    strncpy(s.password, menu.password, MAX_PASS - 1);

    if (menu.mode == MODE_LISTEN) {
        s.is_host = 1;

        int listener = init_listener(port);
        if (listener < 0) { endwin(); return 1; }

        endwin();
        firewall_open(port, disc_port);

        if (tui_lobby(&s, listener, menu.password[0] ? menu.password : NULL) < 0) {
            close(listener);
            firewall_close(port, disc_port);
            discovery_stop();
            return 0;
        }

        /* Pass listener into session — accept_thread in chat.c keeps it open */
        s.listener_fd = listener;

        Packet start = { .type = CHAT_START };
        room_broadcast(&s, &start, -1);

    } else {
        s.is_host = 0;
        char host_nick[MAX_NAME] = {0};

        while (1) {
            const char *pass = tui_enter_password(
                menu.peer_ip[0] ? menu.peer_ip : "unknown", menu.peer_ip);
            int fd = connect_to_peer(menu.peer_ip, port,
                                     menu.nickname, pass, host_nick);
            if (fd == NET_ERR_WRONGPASS) {
                clear();
                mvprintw(LINES/2, (COLS-40)/2, "Wrong password. Press any key to retry.");
                refresh(); getch(); continue;
            }
            if (fd == NET_ERR_REJECTED) {
                clear();
                mvprintw(LINES/2, (COLS-40)/2, "Rejected. Press any key to retry.");
                refresh(); getch(); continue;
            }
            if (fd < 0) { endwin(); return 1; }

            room_add(&s, fd, host_nick[0] ? host_nick : menu.peer_ip);
            discovery_stop();
            break;
        }

        tui_waiting_for_start();
        Packet p;
        while (recv(s.fds[0], &p, sizeof(Packet), 0) > 0) {
            if (p.type == CHAT_START) break;
            if (p.type == PEER_JOIN)
                tui_waiting_for_start_msg(p.content);
        }
    }

    tui_init(menu.nickname);
    start_chat(&s, tui_display_message);
    tui_shutdown();

    if (s.is_host) {
        discovery_stop();
        firewall_close(port, disc_port);
    }

    return 0;
}
