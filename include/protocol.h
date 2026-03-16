#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAX_NAME 32
#define MAX_MSG  512
#define MAX_PASS 8

typedef enum { MSG, CONN_REQUEST, CONN_ACCEPT, CONN_REJECT, CONN_WRONG_PASS } MsgType;

typedef struct {
    MsgType type;
    char sender[MAX_NAME];
    char content[MAX_MSG];
    char password[MAX_PASS];
} Packet;

#endif
