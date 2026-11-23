# Crumple Zone: QNX OS 8.0 target binaries, built with qcc (gcc 12, x86_64).
# Requires the QNX SDP 8.0 environment: source ~/qnx800/qnxsdp-env.sh
#
# Copyright (C) 2026 chinesepowered
# SPDX-License-Identifier: AGPL-3.0-only

ARCH    ?= x86_64
CC      := qcc -Vgcc_nto$(ARCH)
OUT     ?= ../build/$(ARCH)
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Icommon -D_QNX_SOURCE
LDLIBS  := -lm

COMMON_SRC := common/cz_names.c common/cz_rt.c common/cz_bus.c common/cz_child.c \
              common/stats.c common/control.c common/plant.c
COMMON_OBJ := $(patsubst common/%.c,$(OUT)/obj/%.o,$(COMMON_SRC))

PROGS := guardian vehicle_sim wheel_sensors brake_ctl steer_ctl cluster telemetry infotainment czctl rtbench
BINS  := $(addprefix $(OUT)/,$(PROGS))

all: $(BINS)

$(OUT)/obj/%.o: common/%.c common/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/obj/%.o: src/%.c common/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Statically linked: a restarted process skips dynamic linking, which is a
# large part of spawn-to-ready time under emulation. telemetry links
# libsocket, which only exists as a shared library, so it stays dynamic.
$(OUT)/%: $(OUT)/obj/%.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -static -Wl,--no-warn-execstack -o $@ $^ $(LDLIBS)

$(OUT)/telemetry: $(OUT)/obj/telemetry.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -lsocket

clean:
	rm -rf $(OUT)

.PHONY: all clean
