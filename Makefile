# Crumple Zone: top-level build.
#
# Copyright (C) 2026 chinesepowered
# SPDX-License-Identifier: AGPL-3.0-only
#
#   make image    build the QNX binaries with qcc and the QEMU x86_64 image with mkqnximage
#   make run      boot QEMU in the background and start the dashboard on http://127.0.0.1:8080
#   make test     host unit tests, then an on-target smoke test that injects every fault
#   make results  measured run of every fault with one kernel trace each, written to results/
#   make record   record and render demo/video.mp4 from a real run
#   make stop     stop QEMU
#
# Needs the QNX SDP 8.0 environment for image: source ~/qnx800/qnxsdp-env.sh

SHELL := /bin/bash
REPO  := $(abspath .)
HOST_CC ?= gcc

.PHONY: all image qnx run stop console dashboard test test-host test-dashboard test-target results record clean

all: image

qnx:
	@[ -n "$$QNX_TARGET" ] || { echo "source ~/qnx800/qnxsdp-env.sh first"; exit 1; }
	$(MAKE) -C qnx

image: qnx
	./image/build-image.sh

run: dashboard
	./image/run-qemu.sh --bg
	cd dashboard && pnpm start

stop:
	./image/run-qemu.sh --stop

console:
	python3 scripts/console.py

dashboard:
	cd dashboard && pnpm install --frozen-lockfile && pnpm build

test: test-host test-dashboard test-target

test-host: build/host/test_main
	./build/host/test_main

build/host/test_main: tests/host/test_main.c qnx/common/stats.c qnx/common/control.c qnx/common/plant.c qnx/common/cz_names.c qnx/common/*.h
	@mkdir -p build/host
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Iqnx/common -o $@ tests/host/test_main.c \
	    qnx/common/stats.c qnx/common/control.c qnx/common/plant.c qnx/common/cz_names.c -lm

test-dashboard:
	cd dashboard && pnpm install --frozen-lockfile && pnpm test

test-target: dashboard
	./image/run-qemu.sh --bg
	cd dashboard && pnpm smoke

results: dashboard
	./image/run-qemu.sh --bg
	cd dashboard && pnpm results

record: dashboard
	./image/run-qemu.sh --bg
	cd dashboard && pnpm record

clean:
	rm -rf build
