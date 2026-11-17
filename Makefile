# towerwatch - real-time telecom site power monitor for QNX 8.0
#
#   make qnx          build for QNX 8.0 with qcc (source qnxsdp-env.sh first)
#   make qnx QARCH=gcc_ntoaarch64le   Raspberry Pi and other 64-bit Arm targets
#   make linux        host build of towerwatch (logic and timing loop, no pwrsim)
#   make test         unit tests and the end-to-end scenario on the host
#
# SPDX-License-Identifier: Apache-2.0

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
QCC     ?= qcc
QARCH   ?= gcc_ntox86_64
QFLAGS  ?= -V$(QARCH) -O2 -g -Wall -std=gnu11

INC     := -Iinclude
COMMON  := src/plant.c src/protect.c

.PHONY: all linux qnx test clean

all: linux

linux: build/linux/towerwatch build/linux/test_protect

build/linux/towerwatch: src/towerwatch.c $(COMMON) include/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INC) -o $@ src/towerwatch.c $(COMMON) -lpthread -lm

build/linux/test_protect: tests/test_protect.c $(COMMON) include/*.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_protect.c $(COMMON) -lpthread -lm

test: build/linux/test_protect
	./build/linux/test_protect

qnx: build/qnx/$(QARCH)/towerwatch build/qnx/$(QARCH)/pwrsim build/qnx/$(QARCH)/pwrctl

build/qnx/$(QARCH)/towerwatch: src/towerwatch.c $(COMMON) include/*.h
	@mkdir -p $(@D)
	$(QCC) $(QFLAGS) $(INC) -o $@ src/towerwatch.c $(COMMON) -lslog2 -lm

build/qnx/$(QARCH)/pwrsim: src/pwrsim.c $(COMMON) include/*.h
	@mkdir -p $(@D)
	$(QCC) $(QFLAGS) $(INC) -o $@ src/pwrsim.c $(COMMON) -lm

build/qnx/$(QARCH)/pwrctl: src/pwrctl.c src/plant.c include/*.h
	@mkdir -p $(@D)
	$(QCC) $(QFLAGS) $(INC) -o $@ src/pwrctl.c src/plant.c -lm

clean:
	rm -rf build
