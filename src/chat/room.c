#include "room.h"
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

/*
 * room_mu is defined here and declared extern in room.h so that tui_lobby.c
 * (FIX M-8) can acquire it when reading session state.
 */
pthread_mutex_t room_mu = PTHREAD_MUTEX_INITIALIZER;

void room_broadcast(Session *s, Packet *p, int skip_fd) {
    int fds[MAX_CLIENTS];
    int count;

    pthread_mutex_lock(&room_mu);
    count = s->count;
    for (int i = 0; i < count; i++)
        fds[i] = s->fds[i];
    pthread_mutex_unlock(&room_mu);

    for (int i = 0; i < count; i++)
        if (fds[i] != skip_fd)
            send(fds[i], p, sizeof(Packet), 0);
}

void room_remove(Session *s, int fd) {
    pthread_mutex_lock(&room_mu);
    for (int i = 0; i < s->count; i++) {
        if (s->fds[i] == fd) {
            s->fds[i] = s->fds[s->count - 1];
            memcpy(s->nicks[i], s->nicks[s->count - 1], MAX_NAME);
            s->count--;
            break;
        }
    }
    pthread_mutex_unlock(&room_mu);
    close(fd);
}

/*
 * room_add — unconditional add; caller is responsible for prior capacity
 * check.  For atomic check+insert use room_try_add() instead.
 */
int room_add(Session *s, int fd, const char *nick) {
    pthread_mutex_lock(&room_mu);
    if (s->count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&room_mu);
        return -1;
    }
    s->fds[s->count] = fd;
    strncpy(s->nicks[s->count], nick, MAX_NAME - 1);
    s->nicks[s->count][MAX_NAME - 1] = '\0';
    s->count++;
    pthread_mutex_unlock(&room_mu);
    return 0;
}

/*
 * FIX C-4: room_try_add() — atomically check capacity AND insert the new
 * peer inside a single critical section.  This closes the TOCTOU window
 * where two simultaneous connections could both pass the count check and
 * then both call room_add(), overflowing the fds[]/nicks[] arrays.
 *
 * Returns  0 on success,
 *         -1 if the room is full (caller should send CONN_REJECT).
 */
int room_try_add(Session *s, int fd, const char *nick) {
    pthread_mutex_lock(&room_mu);
    if (s->count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&room_mu);
        return -1;
    }
    s->fds[s->count] = fd;
    strncpy(s->nicks[s->count], nick, MAX_NAME - 1);
    s->nicks[s->count][MAX_NAME - 1] = '\0';
    s->count++;
    pthread_mutex_unlock(&room_mu);
    return 0;
}

void room_nick_for_fd(Session *s, int fd, char *out, size_t len) {
    pthread_mutex_lock(&room_mu);
    for (int i = 0; i < s->count; i++) {
        if (s->fds[i] == fd) {
            strncpy(out, s->nicks[i], len - 1);
            out[len - 1] = '\0';
            pthread_mutex_unlock(&room_mu);
            return;
        }
    }
    pthread_mutex_unlock(&room_mu);
    out[0] = '\0';
}

void room_shutdown_all(Session *s) {
    int fds[MAX_CLIENTS];
    int count;

    pthread_mutex_lock(&room_mu);
    count    = s->count;
    for (int i = 0; i < count; i++)
        fds[i] = s->fds[i];
    s->count = 0;
    pthread_mutex_unlock(&room_mu);

    for (int i = 0; i < count; i++) {
        shutdown(fds[i], SHUT_RD);
        close(fds[i]);
    }
}

void room_rename(Session *s, int fd, const char *new_nick) {
    pthread_mutex_lock(&room_mu);
    for (int i = 0; i < s->count; i++) {
        if (s->fds[i] == fd) {
            strncpy(s->nicks[i], new_nick, MAX_NAME - 1);
            s->nicks[i][MAX_NAME - 1] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&room_mu);
}
