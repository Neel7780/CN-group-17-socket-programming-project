CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -pthread -MMD -MP
LDFLAGS = -pthread

COMMON_SRCS = common.c proto.c metrics.c
SERVER_SRCS = server.c topics.c fault.c transfer.c $(COMMON_SRCS)
CLIENT_SRCS = client.c $(COMMON_SRCS)
TEST_SRCS = tests/test_fault.c fault.c common.c

SERVER_OBJS = $(SERVER_SRCS:.c=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)
TEST_OBJS = $(TEST_SRCS:.c=.o)

TARGETS = server client

all: $(TARGETS)

server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

client: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests/test_fault: $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: tests/test_fault
	./tests/test_fault

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f server client tests/test_fault *.o *.d tests/*.o tests/*.d *.csv

-include $(wildcard *.d tests/*.d)

.PHONY: all test clean
