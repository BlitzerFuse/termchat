#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    char *end = key + strlen(key) - 1;
    while (end > key && (*end == ' ' || *end == '\t')) *end-- = '\0';

    while (*val == ' ' || *val == '\t') val++;
    end = val + strlen(val) - 1;
    while (end > val && (*end == ' ' || *end == '\t')) *end-- = '\0';

    if (strcmp(key, "nickname") == 0) {
        strncpy(cfg->nickname, val, MAX_NAME - 1);
    } else if (strcmp(key, "port") == 0) {
        int p = atoi(val);
        if (p > 0 && p <= 65535) cfg->port = p;
    } else if (strcmp(key, "discovery_port") == 0) {
        int p = atoi(val);
        if (p > 0 && p <= 65535) cfg->discovery_port = p;
    }
}

int config_load(Config *cfg) {
    char path[512];
    if (!config_path(path, sizeof(path))) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    while (fgets(line, sizeof(line), f))
        parse_line(cfg, line);

    fclose(f);
    return 0;
}

int config_save(const Config *cfg) {
    char path[512];
    if (!config_path(path, sizeof(path))) return -1;

    const char *home = getenv("HOME");
    if (home) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/.termchan", home);
        mkdir(dir, 0700);
    }

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# term-chan configuration\n");
    fprintf(f, "# Lines starting with # are comments.\n\n");

    if (cfg->nickname[0])
        fprintf(f, "nickname       = %s\n", cfg->nickname);
    else
        fprintf(f, "# nickname     = yourname\n");

    fprintf(f, "port           = %d\n", cfg->port);
    fprintf(f, "discovery_port = %d\n", cfg->discovery_port);

    fclose(f);
    return 0;
}
