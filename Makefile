CC = gcc
CFLAGS = -Wall -Iinclude

SRC = src/main.c \
      src/chat/chat.c \
			src/chat/commands.c \
      src/network/network.c \
      src/network/discovery.c \
      src/tui/tui.c \
			src/tui/tui_menu.c \
			src/tui/tui_chat.c 

OBJ = $(SRC:.c=.o)
TARGET = termchan

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lncurses -lpthread
 
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

disc_debug: tools/debug/disc_debug.c
	$(CC) $(CFLAGS) -o tools/debug/disc_debug tools/debug/disc_debug.c

clean:
	rm -f $(OBJ) $(TARGET)
