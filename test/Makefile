# Host unit tests for the hardware-independent firmware code.
#   make -C test        builds and runs them (needs only gcc and make)
CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g -O0 -fsanitize=address,undefined -I../main

SRCS = test_main.c test_core.c test_net_util.c ../main/artnet_core.c ../main/net_util.c

all: run

test_runner: $(SRCS) test.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: test_runner
	./test_runner

clean:
	rm -f test_runner

.PHONY: all run clean
