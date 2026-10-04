CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

all: keymouse

keymouse: keymouse.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f keymouse

.PHONY: all clean
