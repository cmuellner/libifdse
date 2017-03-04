TOPDIR=$(shell dirname $(abspath $(lastword $(MAKEFILE_LIST))))
SRC_DIR=$(TOPDIR)/src

CC=$(CROSS_COMPILE)gcc
CFLAGS+=-Wall -Wextra -O2
RM=rm -f

export CC CFLAGS RM

all:
	$(MAKE) -C $(SRC_DIR)

clean:
	$(MAKE) -C $(SRC_DIR) clean

