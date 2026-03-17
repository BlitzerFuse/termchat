#include "discovery.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define DISC_PING    "TERMCHAT_DISCOVER"
#define DISC_PONG    "TERMCHAT_HERE"
#define TIMEOUT_SEC  3   /* total scan window */
#define PING_INTERVAL_MS 600  /* re-ping every 600ms within the window */

/* Walk interfaces and return the broadcast address of the first non-loopback,
   broadcast-capable, up interface. Falls back to 255.255.255.255. */
static in_addr_t get_broadcast_addr(void) {
    struct ifaddrs *ifap, *ifa;
    in_addr_t result = INADDR_BROADCAST;

    if (getifaddrs(&ifap) < 0) return result;

    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)    continue;
        if (!(ifa->ifa_flags & IFF_UP))        continue;
        if (!(ifa->ifa_flags & IFF_BROADCAST)) continue;
        if (!ifa->ifa_broadaddr)               continue;

        result = ((struct sockaddr_in *)ifa->ifa_broadaddr)->sin_addr.s_addr;
        break;
    }

    freeifaddrs(ifap);
    return result;
}

/* Get our own real non-loopback IP via getifaddrs so self-filtering works
   correctly. getsockname() on an INADDR_ANY socket returns 0.0.0.0, which
   never matches incoming packets and breaks the self-filter. */
static in_addr_t get_local_addr(void) {
    struct ifaddrs *ifap, *ifa;
    in_addr_t result = 0;

    if (getifaddrs(&ifap) < 0) return result;

    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP))    continue;

        result = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
        break;
    }

    freeifaddrs(ifap);
    return result;
}

static long ms_until(struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (deadline->tv_sec  - now.tv_sec)  * 1000
         + (deadline->tv_nsec - now.tv_nsec) / 1000000;
}

int discover_peers(Peer *peers, int max, const char *my_nickname) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return 0;

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    /* Bind to an ephemeral port — replies come back here */
    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = 0
    };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock); return 0;
    }

    in_addr_t baddr    = get_broadcast_addr();
    in_addr_t self_ip  = get_local_addr();   /* real local IP for self-filter */

    char ping[128];
    snprintf(ping, sizeof(ping), "%s %s", DISC_PING, my_nickname);

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DISCOVERY_PORT),
        .sin_addr.s_addr = baddr
    };

    /* Brief pause before first ping — gives the peer's discovery_respond
       thread time to bind its socket. Without this, the first broadcast
       fires before the listener is ready and gets silently dropped. */
    usleep(150000); /* 150ms */

    /* Deadline for the whole scan */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += TIMEOUT_SEC;

    /* Next time we should send a ping */
    struct timespec next_ping;
    clock_gettime(CLOCK_MONOTONIC, &next_ping);

    int count = 0;
    fd_set fds;

    while (count < max) {
        long ms_left = ms_until(&deadline);
        if (ms_left <= 0) break;

        /* Send a (re)ping if it's time */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms_to_ping = (next_ping.tv_sec  - now.tv_sec)  * 1000
                        + (next_ping.tv_nsec - now.tv_nsec) / 1000000;

        if (ms_to_ping <= 0) {
            sendto(sock, ping, strlen(ping), 0,
                   (struct sockaddr *)&dest, sizeof(dest));
            /* Schedule next ping */
            clock_gettime(CLOCK_MONOTONIC, &next_ping);
            next_ping.tv_sec  += PING_INTERVAL_MS / 1000;
            next_ping.tv_nsec += (PING_INTERVAL_MS % 1000) * 1000000;
            if (next_ping.tv_nsec >= 1000000000L) {
                next_ping.tv_sec++;
                next_ping.tv_nsec -= 1000000000L;
            }
            ms_to_ping = PING_INTERVAL_MS;
        }

        /* Wait for a reply, but no longer than time-to-next-ping */
        long wait_ms = ms_to_ping < ms_left ? ms_to_ping : ms_left;
        struct timeval tv = { wait_ms / 1000, (wait_ms % 1000) * 1000 };
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        char buf[256] = {0};
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;

        /* Discard anything that isn't a pong */
        if (strncmp(buf, DISC_PONG, strlen(DISC_PONG)) != 0) continue;

        /* Discard our own reply using real local IP */
        if (self_ip && from.sin_addr.s_addr == self_ip) continue;
        if ((ntohl(from.sin_addr.s_addr) >> 24) == 127) continue;

        const char *nick = buf + strlen(DISC_PONG);
        while (*nick == ' ') nick++;

        /* Deduplicate by IP */
        int dup = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(peers[i].ip, inet_ntoa(from.sin_addr)) == 0) {
                dup = 1; break;
            }
        }
        if (dup) continue;

        strncpy(peers[count].ip, inet_ntoa(from.sin_addr),
                sizeof(peers[count].ip) - 1);
        snprintf(peers[count].nickname, sizeof(peers[count].nickname), "%.*s",
                 (int)(sizeof(peers[count].nickname) - 1), nick);
        count++;
    }

    close(sock);
    return count;
}

void discovery_respond(const char *my_nickname, volatile int *stop) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Poll every 500ms so we can check the stop flag */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(DISCOVERY_PORT)
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); return;
    }

    while (!stop || !*stop) {
        char buf[256] = {0};
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue; /* timeout or error — loop and check stop */
        if (strncmp(buf, DISC_PING, strlen(DISC_PING)) != 0) continue;

        const char *nick = buf + strlen(DISC_PING);
        while (*nick == ' ') nick++;

        char reply[128];
        snprintf(reply, sizeof(reply), "%s %s", DISC_PONG, my_nickname);
        sendto(sock, reply, strlen(reply), 0,
               (struct sockaddr *)&from, flen);
    }

    close(sock);
}
