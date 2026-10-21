CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -pthread -MMD -MP
LDFLAGS = -pthread

COMMON_SRCS = common.c proto.c metrics.c
CLIENT_SRCS = client.c $(COMMON_SRCS)
SERVER_SRCS = server.c topics.c transfer.c $(COMMON_SRCS)
TEST_SRCS = tests/test_common.c $(COMMON_SRCS)

CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)
SERVER_OBJS = $(SERVER_SRCS:.c=.o)
TEST_OBJS = $(TEST_SRCS:.c=.o)

# The server is built once its sources are present in this folder.
TARGETS = client
ifneq ($(wildcard server.c),)
TARGETS += server
endif

all: $(TARGETS)

client: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test_common: $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: tests/test_common
	./tests/test_common

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f server client tests/test_common *.o *.d tests/*.o tests/*.d *.csv

-include $(wildcard *.d tests/*.d)

.PHONY: all test clean
