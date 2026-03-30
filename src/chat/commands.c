#include "commands.h"
#include "room.h"
#include "tui.h"
#include "tui_internal.h"
#include <string.h>
#include <stdio.h>

static CmdResult handle_quit(const char *args, char *nickname, Session *s) {
    (void)args; (void)nickname; (void)s;
    return CMD_QUIT;
}

static CmdResult handle_nick(const char *args, char *nickname, Session *s) {
    if (!args || args[0] == '\0') { tui_status("Usage: /nick <n>"); return CMD_OK; }

    Packet p = { .type = NICK_CHANGE };
    strncpy(p.sender,  nickname, MAX_NAME - 1);
    strncpy(p.content, args,     MAX_NAME - 1);
    p.sender[MAX_NAME  - 1] = '\0';
    p.content[MAX_NAME - 1] = '\0';

    strncpy(nickname, args, MAX_NAME - 1);
    nickname[MAX_NAME - 1] = '\0';

    room_broadcast(s, &p, -1);

    tui_status("Nickname changed to %s", nickname);
    return CMD_OK;
}

static CmdResult handle_clear(const char *args, char *nickname, Session *s) {
    (void)args; (void)nickname; (void)s;
    tui_clear_chat();
    return CMD_OK;
}

static CmdResult handle_me(const char *args, char *nickname, Session *s) {
    (void)args;
    char ip[64];
    tui_get_local_ip(ip, sizeof(ip));
    tui_status("You are %s | IP: %s | Role: %s | Peers: %d",
               nickname,
               ip,
               s->is_host ? "host" : "guest",
               s->count);
    return CMD_OK;
}

static CmdResult handle_pass(const char *args, char *nickname, Session *s) {
    (void)args; (void)nickname;
    if (!s->is_host) {
        tui_status("Only the host can use /pass.");
        return CMD_OK;
    }
    if (s->password[0])
        tui_status("Session password: %s", s->password);
    else
        tui_status("This session has no password.");
    return CMD_OK;
}

static CmdResult handle_ip(const char *args, char *nickname, Session *s) {
    (void)args; (void)nickname; (void)s;
    char ip[64];
    tui_get_local_ip(ip, sizeof(ip));
    tui_status("Your IP: %s", ip);
    return CMD_OK;
}

static CmdResult handle_reply(const char *args, char *nickname, Session *s) {
    if (!args || args[0] == '\0') { tui_status("Usage: /reply <msg>"); return CMD_OK; }
    const char *target = tui_get_last_sender();
    if (!target) { tui_status("No one to reply to yet."); return CMD_OK; }
    Packet out = { .type = MSG };
    strncpy(out.sender, nickname, MAX_NAME - 1);
    strncpy(out.target, target, MAX_NAME - 1);
    snprintf(out.content, MAX_MSG - 1, "@%s %s", target, args);
    room_broadcast(s, &out, -1);
    tui_display_message(&out);
    return CMD_OK;
}

static CmdResult handle_hideotherpanels(const char *args, char *nickname, Session *s) {
    (void)args; (void)nickname; (void)s;
    tui_toggle_panels();
    return CMD_OK;
}

static CmdResult handle_help(const char *args, char *nickname, Session *s);

static const Command builtins[] = {
    { "quit",  "/quit",          handle_quit  },
    { "nick",  "/nick <n>",      handle_nick  },
    { "clear", "/clear",         handle_clear },
    { "me",    "/me",            handle_me    },
    { "ip",    "/ip",            handle_ip    },
    { "reply", "/reply <msg>",   handle_reply },
    { "pass",  "/pass",          handle_pass  },
    { "help",             "/help",                    handle_help             },
    { "hideotherpanels",  "/hideotherpanels",          handle_hideotherpanels  },
};
#define N_BUILTINS ((int)(sizeof(builtins) / sizeof(builtins[0])))

#define MAX_EXTRA_CMDS 16
static Command extra_cmds[MAX_EXTRA_CMDS];
static int     n_extra = 0;

static CmdResult handle_help(const char *args, char *nickname, Session *s) {
    (void)args; (void)nickname; (void)s;
    char line[256] = {0};
    size_t pos = 0;
    for (int i = 0; i < N_BUILTINS; i++) {
        int n = snprintf(line + pos, sizeof(line) - pos,
                         "%s%s", pos ? "  " : "", builtins[i].usage);
        if (n > 0) pos += (size_t)n;
    }
    for (int i = 0; i < n_extra; i++) {
        int n = snprintf(line + pos, sizeof(line) - pos, "  %s", extra_cmds[i].usage);
        if (n > 0) pos += (size_t)n;
    }
    tui_status("%s", line);
    return CMD_OK;
}

CmdResult cmd_dispatch(const char *input, char *nickname, Session *s) {
    if (!input || input[0] != '/') return CMD_UNKNOWN;
    const char *word  = input + 1;
    const char *space = strchr(word, ' ');
    size_t      wlen  = space ? (size_t)(space - word) : strlen(word);
    const char *args  = space ? space + 1 : word + wlen;

    for (int i = 0; i < N_BUILTINS; i++)
        if (strncmp(builtins[i].name, word, wlen) == 0 && builtins[i].name[wlen] == '\0')
            return builtins[i].handler(args, nickname, s);
    for (int i = 0; i < n_extra; i++)
        if (strncmp(extra_cmds[i].name, word, wlen) == 0 && extra_cmds[i].name[wlen] == '\0')
            return extra_cmds[i].handler(args, nickname, s);

    tui_status("Unknown command '%.*s'. Type /help.", (int)wlen, word);
    return CMD_OK;
}

int cmd_register(const char *name, const char *usage,
                 CmdResult (*handler)(const char *, char *, Session *)) {
    if (n_extra >= MAX_EXTRA_CMDS) return -1;
    extra_cmds[n_extra].name    = name;
    extra_cmds[n_extra].usage   = usage;
    extra_cmds[n_extra].handler = handler;
    n_extra++;
    return 0;
}
