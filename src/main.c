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

/* FIX L-10: explicit_bzero() zeroes memory in a way the compiler cannot
 * optimise away, preventing passwords from lingering in core dumps or
 * swap after the process exits. */
#ifdef __GLIBC__
#  include <string.h>   /* explicit_bzero is in <string.h> on glibc */
#else
#  include <strings.h>
#endif

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-p <port>]\n", prog);
    fprintf(stderr,
            "  -p, --port <port>  TCP port (default: from config or 5000)\n");
}

int main(int argc, char *argv[]) {
    Config cfg;
    config_defaults(&cfg);
    config_load(&cfg);

    int cli_port = 0;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 ||
             strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            cli_port = atoi(argv[++i]);
            if (cli_port <= 0 || cli_port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    MenuResult menu;
    memset(&menu, 0, sizeof(menu));
    strncpy(menu.nickname, cfg.nickname, MAX_NAME - 1);
    menu.port           = cli_port ? cli_port : cfg.port;
    menu.discovery_port = cfg.discovery_port;

    if (tui_menu(&menu) < 0)
        goto cleanup_zero;

    strncpy(cfg.nickname, menu.nickname, MAX_NAME - 1);
    cfg.port = menu.port;
    config_save(&cfg);

    int port      = menu.port;
    int disc_port = menu.discovery_port;

    Session s;
    memset(&s, 0, sizeof(s));
    s.listener_fd = -1;
    strncpy(s.my_nick,  menu.nickname, MAX_NAME - 1);
    strncpy(s.password, menu.password, MAX_PASS - 1);

    if (menu.mode == MODE_LISTEN) {
        s.is_host = 1;

        int listener = init_listener(port);
        if (listener < 0) {
            endwin();
            fprintf(stderr,
                    "Error: could not bind to port %d.\n"
                    "Is another termchan session already running?\n"
                    "Try: ss -tlnp | grep %d\n", port, port);
            goto cleanup_zero;
        }

        endwin();
        firewall_open(port, disc_port);

        if (tui_lobby(&s, listener,
                      menu.password[0] ? menu.password : NULL,
                      port) < 0) {
            close(listener);
            firewall_close(port, disc_port);
            discovery_stop();
            goto cleanup_zero;
        }

        s.listener_fd = listener;

        Packet start;
        memset(&start, 0, sizeof(start));
        start.type = CHAT_START;
        room_broadcast(&s, &start, -1);

    } else {
        s.is_host = 0;
        char host_nick[MAX_NAME] = {0};

        while (1) {
            /*
             * FIX M-5: tui_enter_password() now accepts a caller-owned
             * buffer instead of returning a pointer to a static buffer.
             * This eliminates the silent overwrite that occurred on each
             * retry loop iteration.
             */
            char pass_buf[MAX_PASS + 1];
            memset(pass_buf, 0, sizeof(pass_buf));
            tui_enter_password(
                host_nick[0] ? host_nick : menu.peer_ip,
                menu.peer_ip,
                pass_buf,
                sizeof(pass_buf));

            int fd = connect_to_peer(menu.peer_ip, port,
                                     menu.nickname, pass_buf,
                                     host_nick);

            /* FIX L-10: zero the temporary password buffer immediately
             * after use — don't wait until process exit. */
            explicit_bzero(pass_buf, sizeof(pass_buf));

            if (fd == NET_ERR_WRONGPASS) {
                clear();
                mvprintw(LINES / 2, (COLS - 40) / 2,
                         "Wrong password. Press any key to retry.");
                refresh(); getch();
                continue;
            }
            if (fd == NET_ERR_REJECTED) {
                clear();
                mvprintw(LINES / 2, (COLS - 40) / 2,
                         "Rejected. Press any key to retry.");
                refresh(); getch();
                continue;
            }
            if (fd < 0) { endwin(); goto cleanup_zero; }

            room_add(&s, fd, host_nick[0] ? host_nick : menu.peer_ip);
            discovery_stop();
            break;
        }

        if (tui_waiting_run(s.fds[0], host_nick,
                            menu.peer_ip, port,
                            menu.nickname) < 0) {
            close(s.fds[0]);
            goto cleanup_zero;
        }
    }

    tui_init(menu.nickname, &s);
    start_chat(&s, tui_display_message);
    tui_shutdown();

    if (s.is_host) {
        discovery_stop();
        firewall_close(port, disc_port);
    }

    /* FIX L-10: zero all sensitive password fields before returning.
     * explicit_bzero() is not subject to dead-store elimination by the
     * compiler, unlike memset(). */
cleanup_zero:
    explicit_bzero(s.password,    sizeof(s.password));
    explicit_bzero(menu.password, sizeof(menu.password));

    return 0;
}
