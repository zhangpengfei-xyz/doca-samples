CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror \
	$(shell $(PKG_CONFIG) --cflags libibverbs)
LDLIBS += $(shell $(PKG_CONFIG) --libs libibverbs)

.PHONY: all clean

all: srdma_ctrl_test

srdma_ctrl_test: srdma_ctrl_test.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) srdma_ctrl_test
