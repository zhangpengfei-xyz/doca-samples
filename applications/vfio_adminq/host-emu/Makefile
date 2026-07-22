CC ?= cc
CPPFLAGS += -I../common
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Werror

.PHONY: all clean

all: vfio_adminq_host_emu

vfio_adminq_host_emu: host_emu.c ../common/vfio_adminq_abi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ host_emu.c

clean:
	$(RM) vfio_adminq_host_emu
