#include "chat.h"
#include "commands.h"
#include "tui.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct {
    int socket_fd;
    void (*display_cb)(Packet *);
    atomic_int running;
} RecvArgs;

static void *recv_thread(void *arg) {
    RecvArgs *a = (RecvArgs *)arg;
    Packet p;
    while (atomic_load(&a->running)) {
        if (recv(a->socket_fd, &p, sizeof(Packet), 0) <= 0) {
            tui_status("Peer disconnected.");
            atomic_store(&a->running, 0);
            break;
        }
        if (a->display_cb) a->display_cb(&p);
    }
    return NULL;
}

void start_chat(int socket_fd, char *nickname, void (*display_cb)(Packet *)) {
    RecvArgs args = { socket_fd, display_cb, 1 };

    pthread_t tid;
    if (pthread_create(&tid, NULL, recv_thread, &args) != 0) {
        tui_status("Failed to start receiver thread.");
        close(socket_fd);
        return;
    }

    while (atomic_load(&args.running)) {
        char *input = tui_get_input();
        if (!input) break;  /* NULL = resize signal or I/O error — exit cleanly */
        if (input[0] == '\0') { free(input); continue; }

        if (input[0] == '/') {
            CmdResult r = cmd_dispatch(input, nickname, socket_fd);
            free(input);
            if (r == CMD_QUIT) break;
            continue;
        }

        Packet out = { .type = MSG };
        strncpy(out.sender,  nickname, MAX_NAME - 1);
        strncpy(out.content, input,    MAX_MSG  - 1);
        free(input);

        if (send(socket_fd, &out, sizeof(Packet), 0) <= 0) {
            tui_status("Send failed — connection lost.");
            break;
        }
        if (display_cb) display_cb(&out);
    }

    atomic_store(&args.running, 0);
    shutdown(socket_fd, SHUT_RDWR);
    pthread_join(tid, NULL);
    close(socket_fd);
}
