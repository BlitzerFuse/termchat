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

#define BEACON          "TERMCHAT_BEACON"
#define BEACON_INTERVAL_MS 500
#define PEER_TTL_MS     5000
#define MAX_IFACES      8

typedef struct {
    Peer            p;
    struct timespec last_seen;
    int             active;
} PeerEntry;

static PeerEntry       g_table[MAX_PEERS];
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

typedef struct {
    in_addr_t bcast;
    in_addr_t local;
} IfaceInfo;

static int get_all_ifaces(IfaceInfo *out, int max) {
    struct ifaddrs *ifap, *ifa;
    int count = 0;
    if (getifaddrs(&ifap) < 0) return 0;
    for (ifa = ifap; ifa && count < max; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)    continue;
        if (!(ifa->ifa_flags & IFF_UP))        continue;
        if (!(ifa->ifa_flags & IFF_BROADCAST)) continue;
        if (!ifa->ifa_broadaddr)               continue;
        out[count].bcast = ((struct sockaddr_in *)ifa->ifa_broadaddr)->sin_addr.s_addr;
        out[count].local = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
        count++;
    }
    freeifaddrs(ifap);
    return count;
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
    IfaceInfo ifaces[MAX_IFACES];
    int       send_socks[MAX_IFACES];
    int       n_ifaces = 0;

    IfaceInfo all[MAX_IFACES];
    int n_all = get_all_ifaces(all, MAX_IFACES);

    for (int i = 0; i < n_all && n_ifaces < MAX_IFACES; i++) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) continue;
        int bcast = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

        struct sockaddr_in local = {
            .sin_family      = AF_INET,
            .sin_addr.s_addr = all[i].local,
            .sin_port        = 0              /* ephemeral port */
        };
        if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
            close(s);
            continue;
        }
        ifaces[n_ifaces]      = all[i];
        send_socks[n_ifaces]  = s;
        n_ifaces++;
    }

    if (n_ifaces == 0) {
        return NULL;
    }

    int recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv_sock < 0) {
        for (int i = 0; i < n_ifaces; i++) close(send_socks[i]);
        return NULL;
    }
    int reuse = 1;
    setsockopt(recv_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(recv_sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    struct sockaddr_in recv_local = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(disc_port)
    };
    if (bind(recv_sock, (struct sockaddr *)&recv_local, sizeof(recv_local)) < 0) {
        close(recv_sock);
        for (int i = 0; i < n_ifaces; i++) close(send_socks[i]);
        return NULL;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = BEACON_INTERVAL_MS * 1000 };
    setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char beacon[128];
    snprintf(beacon, sizeof(beacon), "%s %s", BEACON, a->nickname);
    size_t beacon_len = strlen(beacon);

    while (!a->stop) {
        for (int i = 0; i < n_ifaces; i++) {
            struct sockaddr_in dest = {
                .sin_family      = AF_INET,
                .sin_port        = htons(disc_port),
                .sin_addr.s_addr = ifaces[i].bcast
            };
            sendto(send_socks[i], beacon, beacon_len, 0,
                   (struct sockaddr *)&dest, sizeof(dest));
        }

        char buf[256] = {0};
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(recv_sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        if (strncmp(buf, BEACON, strlen(BEACON)) != 0) continue;
        if (is_local_addr(from.sin_addr.s_addr))       continue;
        if ((ntohl(from.sin_addr.s_addr) >> 24) == 127) continue;

        const char *nick = buf + strlen(BEACON);
        while (*nick == ' ') nick++;
        if (*nick == '\0') continue;

        table_upsert(inet_ntoa(from.sin_addr), nick);
    }

    close(recv_sock);
    for (int i = 0; i < n_ifaces; i++) close(send_socks[i]);
    return NULL;
}

void discovery_start(const char *my_nickname, int port) {
    if (g_beacon) return;

    memset(g_table, 0, sizeof(g_table));

    g_beacon = malloc(sizeof(BeaconArgs));
    if (!g_beacon) return;
    strncpy(g_beacon->nickname, my_nickname, MAX_NAME - 1);
    g_beacon->nickname[MAX_NAME - 1] = '\0';
    g_beacon->disc_port = port > 0 ? port : DISCOVERY_PORT;
    g_beacon->stop      = 0;

    pthread_create(&g_beacon_tid, NULL, beacon_thread, g_beacon);
}

void discovery_stop(void) {
    if (!g_beacon) return;
    g_beacon->stop = 1;
    pthread_join(g_beacon_tid, NULL);
    free(g_beacon);
    g_beacon = NULL;
}
