CC     = gcc
CFLAGS = -Wall -Iinclude

SRC = src/main.c \
      src/chat/chat.c \
      src/chat/commands.c \
      src/chat/room.c \
      src/network/network.c \
      src/network/discovery.c \
      src/network/firewall.c \
      src/tui/tui.c \
      src/tui/tui_menu.c \
      src/tui/tui_chat.c \
      src/tui/tui_lobby.c \
      src/config.c

OBJ    = $(SRC:.c=.o)
TARGET = termchan

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lncurses -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
