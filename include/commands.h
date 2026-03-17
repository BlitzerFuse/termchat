#ifndef COMMANDS_H
#define COMMANDS_H

#include "protocol.h"
#include <stddef.h>

/* Return codes for command handlers */
typedef enum {
    CMD_OK,       /* command handled, continue loop  */
    CMD_QUIT,     /* /quit — caller should break      */
    CMD_UNKNOWN   /* not a registered command         */
} CmdResult;

/*
 * A single command entry.
 *   name    — the slash-word, e.g. "quit"  (no leading slash)
 *   usage   — shown by /help, e.g. "/quit"
 *   handler — called with (args, nickname, socket_fd)
 *             args is the remainder of the input after the command word
 *             (may be an empty string, never NULL)
 *             socket_fd is -1 when no live connection is available
 */
typedef struct {
    const char *name;
    const char *usage;
    CmdResult (*handler)(const char *args, char *nickname, int socket_fd);
} Command;

/*
 * Dispatch an input line that begins with '/'.
 * Returns CMD_QUIT if the session should end, CMD_OK otherwise.
 * Prints status/help messages itself via tui_status().
 */
CmdResult cmd_dispatch(const char *input, char *nickname, int socket_fd);

/* Register a custom command at runtime (max 16 extras). */
int cmd_register(const char *name, const char *usage,
                 CmdResult (*handler)(const char *, char *, int));

#endif /* COMMANDS_H */
