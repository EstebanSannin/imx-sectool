#!/usr/bin/env python3
#
# uboot_capture.py -- stop U-Boot autoboot on the serial console, run the
# Phase 0 reference commands (fuse read, md of ROM/OCRAM/SRC/SNVS), then boot.
#
# usage: uboot_capture.py <tty> <logfile> [--at-prompt]
#   Start it, then reboot the board. With --at-prompt, U-Boot is already
#   waiting at its prompt and the autoboot interruption is skipped.
#
# License: MIT.  Author: Stefano Viola <stefano.viola@toradex.com>
import os, re, sys, termios, time

if len(sys.argv) < 3:
    sys.exit("usage: uboot_capture.py <tty> <logfile> [--at-prompt]")
TTY = sys.argv[1]
LOG = sys.argv[2]
CMDS = [
    "version",
    "fuse read 0 0 4", "fuse read 1 0 4", "fuse read 6 0 4", "fuse read 7 0 4",
    "fuse read 4 0 4", "fuse read 8 0 4",
    "hab_status",
    "md.l 0x900 0x10", "md.l 0x9e0 1",
    "md.l 0x90d040 0x60",
    "md.l 0x9112f8 0x20",
    "md.l 0x30390058 1", "md.l 0x3039005c 1", "md.l 0x30390070 1", "md.l 0x30390098 1",
    "md.l 0x30370014 1",
    "printenv bootdelay",
]
PROMPT = re.compile(rb"(=> |# ) *$")

fd = os.open(TTY, os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 1
termios.tcsetattr(fd, termios.TCSANOW, a)
log = open(LOG, "wb")
buf = b""

def pump(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        d = os.read(fd, 4096)
        if d:
            buf += d; log.write(d); log.flush()

def wait_prompt(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        pump(0.2)
        if PROMPT.search(buf.rstrip(b"\r\n") + b" ") or PROMPT.search(buf):
            return True
    return False

# phase 1: wait for U-Boot banner, then hammer keys until the prompt shows
# ("--at-prompt": U-Boot is already waiting, skip straight to the commands)
end = 0 if "--at-prompt" in sys.argv else time.time() + 120
seen = False
while time.time() < end:
    pump(0.05)
    if not seen and (b"U-Boot" in buf or b"autoboot" in buf):
        seen = True
    if seen:
        if PROMPT.search(buf):
            break
        os.write(fd, b" ")
if end and time.time() >= end:
    log.write(b"\n#### TIMEOUT: no U-Boot prompt\n"); sys.exit(2)
os.write(fd, b"\x15")        # Ctrl-U: drop any keys typed after the prompt

os.write(fd, b"\r"); time.sleep(0.3); pump(0.5)
for c in CMDS:
    buf = b""
    log.write(("\n#### CMD %s\n" % c).encode()); log.flush()
    os.write(fd, c.encode() + b"\r")
    if not wait_prompt(15):
        log.write(b"\n#### TIMEOUT on command\n")
buf = b""
log.write(b"\n#### CMD boot\n")
os.write(fd, b"boot\r")
pump(60)
log.write(b"\n#### DONE\n")
