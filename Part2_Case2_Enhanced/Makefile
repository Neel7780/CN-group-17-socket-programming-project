CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -pthread
LDFLAGS = -pthread

COMMON_SRCS = common.c topics.c proto.c metrics.c fault.c session.c transfer.c checkpoint.c manifest.c workq.c
SERVER_SRCS = server.c $(COMMON_SRCS)
CLIENT_SRCS = client.c $(COMMON_SRCS)

SERVER_OBJS = $(SERVER_SRCS:.c=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

all: server client

server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

client: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f server client *.o *.csv

.PHONY: all clean
