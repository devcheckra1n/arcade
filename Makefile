CFLAGS ?= -O2 -Wall -Wextra -Wno-format-truncation

all: pack covers

pack: pack.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libcrypto) -o $@ $< $(shell pkg-config --libs libcrypto)

covers: covers.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags libcurl zlib) -o $@ $< $(shell pkg-config --libs libcurl zlib) -lm

clean:
	rm -f pack covers

.PHONY: all clean
