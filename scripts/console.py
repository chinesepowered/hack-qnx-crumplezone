#!/usr/bin/env python3
# Crumple Zone: attach this terminal to the QNX serial console that QEMU
# exposes on TCP (image/run-qemu.sh --bg). Ctrl-] quits.
#
# Copyright (C) 2026 chinesepowered
# SPDX-License-Identifier: AGPL-3.0-only
import os
import select
import socket
import sys
import termios
import tty

port = int(os.environ.get("CZ_CONSOLE_PORT", "4555"))
sock = socket.create_connection(("127.0.0.1", port))
fd = sys.stdin.fileno()
old = termios.tcgetattr(fd)
print(f"connected to the QNX console on 127.0.0.1:{port}; Ctrl-] quits\r")
try:
    tty.setraw(fd)
    sock.sendall(b"\r")
    while True:
        r, _, _ = select.select([sock, fd], [], [])
        if sock in r:
            data = sock.recv(4096)
            if not data:
                break
            os.write(sys.stdout.fileno(), data)
        if fd in r:
            data = os.read(fd, 1024)
            if b"\x1d" in data:
                break
            sock.sendall(data)
finally:
    termios.tcsetattr(fd, termios.TCSADRAIN, old)
    sock.close()
    print()
