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
#include <pthread.h>
#include <stdlib.h>

static void blog(const char *msg) {
    FILE *f = fopen("/tmp/tc_beacon.log", "a");
    if (!f) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(f, "[%ld.%03ld] %s\n", t.tv_sec, t.tv_nsec / 1000000, msg);
    fclose(f);
}

#define BEACON          "TERMCHAT_BEACON"
#define BEACON_INTERVAL_MS 500
#define PEER_TTL_MS     5000

typedef struct {
    Peer            p;
    struct timespec last_seen;
    int             active;
} PeerEntry;

static PeerEntry    g_table[MAX_PEERS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static long ms_since(struct timespec *t) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec  - t->tv_sec)  * 1000
         + (now.tv_nsec - t->tv_nsec) / 1000000;
}

static void table_upsert(const char *ip, const char *nick) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_table[i].active &&
            strcmp(g_table[i].p.ip, ip) == 0) {
            g_table[i].last_seen = now;
            strncpy(g_table[i].p.nickname, nick, MAX_NAME - 1);
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!g_table[i].active) {
            strncpy(g_table[i].p.ip,       ip,   sizeof(g_table[i].p.ip)       - 1);
            strncpy(g_table[i].p.nickname, nick, sizeof(g_table[i].p.nickname) - 1);
            g_table[i].last_seen = now;
            g_table[i].active    = 1;
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

static void table_expire(void) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_table[i].active && ms_since(&g_table[i].last_seen) > PEER_TTL_MS)
            g_table[i].active = 0;
    pthread_mutex_unlock(&g_mu);
}

int discovery_peers(Peer *peers, int max) {
    table_expire();
    pthread_mutex_lock(&g_mu);
    int count = 0;
    for (int i = 0; i < MAX_PEERS && count < max; i++)
        if (g_table[i].active)
            peers[count++] = g_table[i].p;
    pthread_mutex_unlock(&g_mu);
    return count;
}

void discovery_reset(void) {
    pthread_mutex_lock(&g_mu);
    memset(g_table, 0, sizeof(g_table));
    pthread_mutex_unlock(&g_mu);
}

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

static int is_local_addr(in_addr_t addr) {
    struct ifaddrs *ifap, *ifa;
    if (getifaddrs(&ifap) < 0) return 0;
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr == addr) {
            freeifaddrs(ifap);
            return 1;
        }
    }
    freeifaddrs(ifap);
    return 0;
}

typedef struct {
    char         nickname[MAX_NAME];
    int          disc_port;
    volatile int stop;
} BeaconArgs;

static BeaconArgs  *g_beacon = NULL;
static pthread_t    g_beacon_tid;

static void *beacon_thread(void *arg) {
    BeaconArgs *a = arg;
    int disc_port = a->disc_port;

    /* Send socket: ephemeral port — our own broadcasts never loop back
       to the recv socket, eliminating self-reception entirely. */
    int send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sock < 0) return NULL;
    int bcast = 1;
    setsockopt(send_sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    struct sockaddr_in send_local = {
        .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = 0
    };
    if (bind(send_sock, (struct sockaddr *)&send_local, sizeof(send_local)) < 0) {
        blog("FAIL: send_sock bind failed"); close(send_sock); return NULL;
    }
    blog("OK: send_sock bound");

    /* Recv socket: bound to disc_port to receive beacons from other peers. */
    int recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_sock < 0) { close(send_sock); return NULL; }
    int reuse = 1;
    setsockopt(recv_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(recv_sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    struct sockaddr_in recv_local = {
        .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
        .sin_port   = htons(disc_port)
    };
    if (bind(recv_sock, (struct sockaddr *)&recv_local, sizeof(recv_local)) < 0) {
        blog("FAIL: recv_sock bind failed"); close(send_sock); close(recv_sock); return NULL;
    }
    blog("OK: recv_sock bound");
    struct timeval tv = { .tv_sec = 0, .tv_usec = BEACON_INTERVAL_MS * 1000 };
    setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    in_addr_t baddr = get_broadcast_addr();
    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(disc_port),
        .sin_addr.s_addr = baddr
    };

    char beacon[128];
    snprintf(beacon, sizeof(beacon), "%s %s", BEACON, a->nickname);

    blog("OK: beacon loop starting");
    while (!a->stop) {
        blog("SEND beacon");
        sendto(send_sock, beacon, strlen(beacon), 0,
               (struct sockaddr *)&dest, sizeof(dest));

        char buf[256] = {0};
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(recv_sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        if (strncmp(buf, BEACON, strlen(BEACON)) != 0) continue;
        if (is_local_addr(from.sin_addr.s_addr)) continue;
        if ((ntohl(from.sin_addr.s_addr) >> 24) == 127) continue;

        const char *nick = buf + strlen(BEACON);
        while (*nick == ' ') nick++;
        if (*nick == '\0') continue;

        blog("RECV peer beacon"); table_upsert(inet_ntoa(from.sin_addr), nick);
    }

    blog("EXIT: beacon_thread exiting");
    close(send_sock);
    close(recv_sock);
    return NULL;
}

// Public API
void discovery_start(const char *my_nickname, int port) {
    if (g_beacon) return;

    memset(g_table, 0, sizeof(g_table));

    g_beacon = malloc(sizeof(BeaconArgs));
    if (!g_beacon) return;
    strncpy(g_beacon->nickname, my_nickname, MAX_NAME - 1);
    g_beacon->nickname[MAX_NAME - 1] = '\0';
    g_beacon->disc_port = port > 0 ? port : DISCOVERY_PORT;
    g_beacon->stop = 0;

    pthread_create(&g_beacon_tid, NULL, beacon_thread, g_beacon);
}

void discovery_stop(void) {
    if (!g_beacon) return;
    g_beacon->stop = 1;
    pthread_join(g_beacon_tid, NULL);
    free(g_beacon);
    g_beacon = NULL;
}
