#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define DEFAULT_CHAT_PORT      5000
#define DEFAULT_DISCOVERY_PORT 5051

void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->port           = DEFAULT_CHAT_PORT;
    cfg->discovery_port = DEFAULT_DISCOVERY_PORT;
}

static char *config_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    snprintf(buf, len, "%s%s", home, CONFIG_PATH);
    return buf;
}

/*
 * FIX M-9: parse_port() replaces bare atoi() with strtol() plus full
 * error and range checking.  Returns the port on success, or -1 if the
 * value is non-numeric, out of range, or has trailing garbage.
 */
static int parse_port(const char *val) {
    if (!val || val[0] == '\0') return -1;
    char *end_ptr;
    errno = 0;
    long p = strtol(val, &end_ptr, 10);
    if (errno != 0 || end_ptr == val || *end_ptr != '\0')
        return -1;
    if (p <= 0 || p > 65535)
        return -1;
    return (int)p;
}

static void parse_line(Config *cfg, char *line) {
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    char *eq = strchr(line, '=');
    if (!eq) return;

    *eq = '\0';
    char *key = line;
    char *val = eq + 1;

    while (*key == ' ' || *key == '\t') key++;
    char *end = key + strlen(key);
    while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *(--end) = '\0';

    while (*val == ' ' || *val == '\t') val++;
    end = val + strlen(val);
    while (end > val && (end[-1] == ' ' || end[-1] == '\t')) *(--end) = '\0';

    if (strcmp(key, "nickname") == 0) {
        strncpy(cfg->nickname, val, MAX_NAME - 1);
        cfg->nickname[MAX_NAME - 1] = '\0';
    } else if (strcmp(key, "port") == 0) {
        int p = parse_port(val);
        if (p > 0)
            cfg->port = p;
        else
            fprintf(stderr,
                    "config: invalid port value '%s', using default %d\n",
                    val, cfg->port);
    } else if (strcmp(key, "discovery_port") == 0) {
        int p = parse_port(val);
        if (p > 0)
            cfg->discovery_port = p;
        else
            fprintf(stderr,
                    "config: invalid discovery_port value '%s', "
                    "using default %d\n", val, cfg->discovery_port);
    }
}

int config_load(Config *cfg) {
    char path[512];
    if (!config_path(path, sizeof(path))) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return 0;   /* no config yet — silently use defaults */

    char line[256];
    while (fgets(line, sizeof(line), f))
        parse_line(cfg, line);

    fclose(f);
    return 0;
}

/*
 * FIX L-8: config_save() now writes to a temporary file in the same
 * directory and renames it into place atomically.  This prevents a
 * partial write (e.g. from a crash or power loss between fopen() and
 * fclose()) from leaving the config file empty or corrupted.
 *
 * rename() within the same filesystem is guaranteed atomic by POSIX.
 */
int config_save(const Config *cfg) {
    char path[512];
    if (!config_path(path, sizeof(path))) return -1;

    /* Ensure the config directory exists. */
    const char *home = getenv("HOME");
    if (home) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/.termchan", home);
        mkdir(dir, 0700);
    }

    /* Write to a temporary file in the same directory. */
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    fprintf(f, "# term-chan configuration\n");
    fprintf(f, "# Lines starting with # are comments.\n\n");

    if (cfg->nickname[0])
        fprintf(f, "nickname       = %s\n", cfg->nickname);
    else
        fprintf(f, "# nickname     = yourname\n");

    fprintf(f, "port           = %d\n", cfg->port);
    fprintf(f, "discovery_port = %d\n", cfg->discovery_port);

    if (fclose(f) != 0) {
        remove(tmp_path);
        return -1;
    }

    /* Atomic replace: if rename() fails, remove the temp file and bail. */
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return -1;
    }

    return 0;
}
