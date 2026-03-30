#include "firewall.h"
#include <stdio.h>
#include <stdlib.h>

static int has_cmd(const char *cmd) {
    char buf[128];
    snprintf(buf, sizeof(buf), "command -v %s >/dev/null 2>&1", cmd);
    return system(buf) == 0;
}

static int ufw_active(void) {
    return system("ufw status 2>/dev/null | grep -q 'Status: active'") == 0;
}

static int firewalld_active(void) {
    return system("firewall-cmd --state >/dev/null 2>&1") == 0;
}

void firewall_open(int tcp_port, int udp_port) {
    char cmd[256];

    if (has_cmd("ufw") && ufw_active()) {
        snprintf(cmd, sizeof(cmd),
                 "ufw allow %d/tcp >/dev/null 2>&1", tcp_port);
        system(cmd);
        snprintf(cmd, sizeof(cmd),
                 "ufw allow %d/udp >/dev/null 2>&1", udp_port);
        system(cmd);
        return;
    }

    if (has_cmd("firewall-cmd") && firewalld_active()) {
        snprintf(cmd, sizeof(cmd),
                 "firewall-cmd --add-port=%d/tcp >/dev/null 2>&1", tcp_port);
        system(cmd);
        snprintf(cmd, sizeof(cmd),
                 "firewall-cmd --add-port=%d/udp >/dev/null 2>&1", udp_port);
        system(cmd);
    }
}

void firewall_close(int tcp_port, int udp_port) {
    char cmd[256];

    if (has_cmd("ufw") && ufw_active()) {
        snprintf(cmd, sizeof(cmd),
                 "ufw delete allow %d/tcp >/dev/null 2>&1", tcp_port);
        system(cmd);
        snprintf(cmd, sizeof(cmd),
                 "ufw delete allow %d/udp >/dev/null 2>&1", udp_port);
        system(cmd);
        return;
    }

    if (has_cmd("firewall-cmd") && firewalld_active()) {
        snprintf(cmd, sizeof(cmd),
                 "firewall-cmd --remove-port=%d/tcp >/dev/null 2>&1", tcp_port);
        system(cmd);
        snprintf(cmd, sizeof(cmd),
                 "firewall-cmd --remove-port=%d/udp >/dev/null 2>&1", udp_port);
        system(cmd);
    }
}
