TOPDIR=$(shell dirname $(abspath $(lastword $(MAKEFILE_LIST))))
SRC_DIR=$(TOPDIR)/src

CC=$(CROSS_COMPILE)gcc
CFLAGS+=-Wall -Wextra -Werror -O2 -ggdb
RM=rm -f

export CC CFLAGS RM

all:
	$(MAKE) -C $(SRC_DIR)

clean:
	$(MAKE) -C $(SRC_DIR) clean

