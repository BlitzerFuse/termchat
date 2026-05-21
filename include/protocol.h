#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_NAME     32
#define MAX_MSG      512
#define MAX_PASS     8   /* buffer size including null terminator */
#define PASSWORD_LEN 6   /* actual usable password length          */

/*
 * Wire format is a packed, fixed-size binary frame.
 * __attribute__((packed)) removes inter-field padding so the layout is
 * identical on every platform/compiler that targets the same arch.
 * Always transmit exactly sizeof(Packet) bytes.
 *
 * FIX C-5: added __attribute__((packed)) and changed type field to
 *          uint32_t so out-of-range enum values sent by a remote peer
 *          are caught by PACKET_TYPE_VALID() before any dispatch.
 */
typedef enum {
    MSG             = 0,
    CONN_REQUEST    = 1,
    CONN_ACCEPT     = 2,
    CONN_REJECT     = 3,
    CONN_WRONG_PASS = 4,
    CHAT_START      = 5,
    PEER_JOIN       = 6,
    PEER_LEAVE      = 7,
    NICK_CHANGE     = 8,
    ROSTER_SYNC     = 9,  /* host->guest: newline-sep peer list in content,
                             host nick in sender */
    MSG_TYPE_MAX    = 9
} MsgType;

/* FIX C-5: validate before any switch/dispatch on a received packet. */
#define PACKET_TYPE_VALID(t) ((uint32_t)(t) <= (uint32_t)MSG_TYPE_MAX)

typedef struct __attribute__((packed)) {
    uint32_t type;              /* one of MsgType; always validate with
                                   PACKET_TYPE_VALID() after recv()      */
    char sender[MAX_NAME];
    char target[MAX_NAME];
    char content[MAX_MSG];
    char password[MAX_PASS];
} Packet;

#endif /* PROTOCOL_H */
