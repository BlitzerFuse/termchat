#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <ifaddrs.h>

#define PORT 5051

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
        printf("[info] using interface %s, broadcast %s\n",
            ifa->ifa_name, inet_ntoa(*(struct in_addr*)&result));
        break;
    }
    freeifaddrs(ifap);
    return result;
}

void do_listen(const char *nick) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = { .sin_family=AF_INET, .sin_addr.s_addr=INADDR_ANY, .sin_port=htons(PORT) };
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("bind failed: %s\n", strerror(errno)); return;
    }
    printf("LISTEN bound to 0.0.0.0:%d — waiting for ping...\n", PORT);

    char buf[256] = {0};
    struct sockaddr_in from; socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&from, &flen);
    printf("LISTEN got %zd bytes from %s:%d : '%s'\n",
        n, inet_ntoa(from.sin_addr), ntohs(from.sin_port), buf);

    char reply[128];
    snprintf(reply, sizeof(reply), "TERMCHAT_HERE %s", nick);
    ssize_t s = sendto(sock, reply, strlen(reply), 0, (struct sockaddr*)&from, flen);
    printf("LISTEN sent reply (%zd bytes) to %s:%d\n", s, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
    close(sock);
}

void do_scan(const char *nick) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    struct sockaddr_in local = { .sin_family=AF_INET, .sin_addr.s_addr=INADDR_ANY, .sin_port=0 };
    bind(sock, (struct sockaddr*)&local, sizeof(local));
    socklen_t slen = sizeof(local);
    getsockname(sock, (struct sockaddr*)&local, &slen);
    printf("SCAN ephemeral port %d\n", ntohs(local.sin_port));

    in_addr_t baddr = get_broadcast_addr();
    struct sockaddr_in dest = { .sin_family=AF_INET, .sin_port=htons(PORT), .sin_addr.s_addr=baddr };
    char ping[128];
    snprintf(ping, sizeof(ping), "TERMCHAT_DISCOVER %s", nick);
    ssize_t s = sendto(sock, ping, strlen(ping), 0, (struct sockaddr*)&dest, sizeof(dest));
    printf("SCAN sent %zd bytes to %s:%d\n", s, inet_ntoa(dest.sin_addr), PORT);

    struct timeval tv = {3, 0};
    fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
    printf("SCAN waiting 3s...\n");
    int r = select(sock+1, &fds, NULL, NULL, &tv);
    if (r <= 0) { printf("SCAN timed out — no reply\n"); close(sock); return; }

    char buf[256] = {0};
    struct sockaddr_in from; socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&from, &flen);
    printf("SCAN got reply from %s: '%s'\n", inet_ntoa(from.sin_addr), buf);
    close(sock);
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: %s listen|scan <nickname>\n", argv[0]); return 1; }
    if (strcmp(argv[1], "listen") == 0) do_listen(argv[2]);
    else do_scan(argv[2]);
    return 0;
}
