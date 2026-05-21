#include "firewall.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * FIX C-1 & C-2:
 *   All previous system() calls have been replaced with fork()/execvp()
 *   so that no shell is ever spawned.  This eliminates:
 *     - command injection via shell metacharacters (C-1)
 *     - hostile binary substitution through PATH manipulation (C-2)
 *
 *   Tool presence is detected using access() against fixed absolute paths
 *   rather than via a shell "command -v" probe (C-2).
 * -----------------------------------------------------------------------*/

/* Known absolute paths for each firewall tool. */
static const char *UFW_PATHS[] = {
    "/usr/sbin/ufw", "/sbin/ufw", NULL
};

static const char *FIREWALLD_PATHS[] = {
    "/usr/bin/firewall-cmd", "/bin/firewall-cmd",
    "/usr/sbin/firewall-cmd", NULL
};

/* Return the first existing, executable path from a NULL-terminated list,
 * or NULL if none is found. */
static const char *find_tool(const char *const *paths) {
    for (int i = 0; paths[i] != NULL; i++)
        if (access(paths[i], X_OK) == 0)
            return paths[i];
    return NULL;
}

/* Run an external command without a shell.  argv must be NULL-terminated.
 * Returns the exit status (0 = success), or -1 on fork/exec failure. */
static int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("firewall: fork");
        return -1;
    }
    if (pid == 0) {
        /* Child: redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);   /* NOLINT */
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);   /* execvp failed */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Check whether ufw is active by probing its status without a shell. */
static int ufw_active(const char *ufw_path) {
    /* "ufw status" prints "Status: active" when enabled. We parse stdout
     * in a child process via a pipe to avoid system(). */
    int pipefd[2];
    if (pipe(pipefd) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return 0; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        char *argv[] = { (char *)ufw_path, "status", NULL };
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);

    char buf[256] = {0};
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    if (n <= 0) return 0;
    return (strstr(buf, "Status: active") != NULL) ? 1 : 0;
}

/* Check whether firewalld is running. */
static int firewalld_active(const char *fwcmd_path) {
    char *argv[] = { (char *)fwcmd_path, "--state", NULL };
    return run_cmd(argv) == 0 ? 1 : 0;
}

void firewall_open(int tcp_port, int udp_port) {
    char tcp_str[20], udp_str[20];
    snprintf(tcp_str, sizeof(tcp_str), "%d/tcp", tcp_port);
    snprintf(udp_str, sizeof(udp_str), "%d/udp", udp_port);

    const char *ufw = find_tool(UFW_PATHS);
    if (ufw && ufw_active(ufw)) {
        char *a_tcp[] = { (char *)ufw, "allow", tcp_str, NULL };
        char *a_udp[] = { (char *)ufw, "allow", udp_str, NULL };
        run_cmd(a_tcp);
        run_cmd(a_udp);
        return;
    }

    const char *fwcmd = find_tool(FIREWALLD_PATHS);
    if (fwcmd && firewalld_active(fwcmd)) {
        char tcp_arg[32], udp_arg[32];
        snprintf(tcp_arg, sizeof(tcp_arg), "--add-port=%s", tcp_str);
        snprintf(udp_arg, sizeof(udp_arg), "--add-port=%s", udp_str);
        char *a_tcp[] = { (char *)fwcmd, tcp_arg, NULL };
        char *a_udp[] = { (char *)fwcmd, udp_arg, NULL };
        run_cmd(a_tcp);
        run_cmd(a_udp);
    }
}

void firewall_close(int tcp_port, int udp_port) {
    char tcp_str[20], udp_str[20];
    snprintf(tcp_str, sizeof(tcp_str), "%d/tcp", tcp_port);
    snprintf(udp_str, sizeof(udp_str), "%d/udp", udp_port);

    const char *ufw = find_tool(UFW_PATHS);
    if (ufw && ufw_active(ufw)) {
        char *a_tcp[] = { (char *)ufw, "delete", "allow", tcp_str, NULL };
        char *a_udp[] = { (char *)ufw, "delete", "allow", udp_str, NULL };
        run_cmd(a_tcp);
        run_cmd(a_udp);
        return;
    }

    const char *fwcmd = find_tool(FIREWALLD_PATHS);
    if (fwcmd && firewalld_active(fwcmd)) {
        char tcp_arg[32], udp_arg[32];
        snprintf(tcp_arg, sizeof(tcp_arg), "--remove-port=%s", tcp_str);
        snprintf(udp_arg, sizeof(udp_arg), "--remove-port=%s", udp_str);
        char *a_tcp[] = { (char *)fwcmd, tcp_arg, NULL };
        char *a_udp[] = { (char *)fwcmd, udp_arg, NULL };
        run_cmd(a_tcp);
        run_cmd(a_udp);
    }
}
