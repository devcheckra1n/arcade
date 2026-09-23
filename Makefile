CFLAGS ?= -O2 -Wall -Wextra -Wno-format-truncation
CFLAGS += $(shell pkg-config --cflags libcrypto)
LDLIBS += $(shell pkg-config --libs libcrypto)

pack: pack.c

clean:
	rm -f pack

.PHONY: clean
