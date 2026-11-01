#!/usr/bin/env python3
#
# mkfixtures.py -- build synthetic i.MX8M fixtures for the host tests.
#
# usage: mkfixtures.py <outdir>
#
# Values mirror a real Verdin iMX8M Plus (docs/phase0-imx8mp.md) except the
# UID words, which are fake. One directory per scenario:
#   open/        open device, unsigned image: 4 HAB events, primary boot
#   closed/      closed device, trusted state, no HAB events
#   fallback/    ROM log with a fallback to the secondary image
#   clobbered/   HAB persistent area overwritten
#   mm-open/     i.MX8M Mini, open, fuses as on a real Verdin iMX8MM
#   mm-auth/     i.MX8M Mini, closed, Secure JTAG (authenticated)
#   mx7-open/    i.MX7D, open, fuses and HAB store as on a real Colibri iMX7D
#   mx7-closed/  i.MX7D, closed, no debug, no HAB events
#
# License: MIT.  Author: Stefano Viola <stefano.viola@toradex.com>
import os, struct, sys

WORDS_PER_BANK = 4
BANKS = 96


def words_to_bytes(words):
    return struct.pack("<%dI" % len(words), *words)


def put(buf, off, words):
    b = words_to_bytes(words)
    buf[off:off + len(b)] = b


def nvmem(closed, debug=None):
    fuses = [0] * (BANKS * WORDS_PER_BANK)
    fuses[0:4] = [0xffafa9eb, 0x0002aaff, 0x11111111, 0x22222222]   # fake UID
    fuses[4:8] = [0x000247d1, 0x01000000, 0x0000007f, 0x98002000]
    if closed:
        fuses[7] |= 0x02000000                                       # SEC_CONFIG
        fuses[24:32] = [0xa5a5a5a5] * 8                              # SRK hash
    for i in range(16, 24):                                          # banks 4-5
        fuses[i] = 0xbadabada
    fuses[35] = 0x00000004
    fuses[36:39] = [0x02030405, 0x0a0b0001, 0x10111213]             # fake MACs
    if closed:
        fuses[0] |= 0x00000200                                       # CST_SRK_LOCK
    # secure debug, as meta-toradex-security's fuse-cmds.txt would burn it
    if debug == "no-debug":
        fuses[7] |= 0x00C00000 | 0x00100000                          # JTAG_SMODE=11, KTE
    elif debug == "jtag-disabled":
        fuses[7] |= 0x00200000                                       # SJC_DISABLE
    elif debug == "authenticated":
        fuses[7] |= 0x00400000 | 0x00100000                          # JTAG_SMODE=01, KTE
        fuses[32:34] = [0xbadabada, 0xbadabada]                      # SJC_RESP, read-locked
    elif debug == "no-debug-nokte":
        fuses[7] |= 0x00C00000
        fuses[0] &= ~0x00000008                                      # BOOT_CFG_LOCK not OP
    return words_to_bytes(fuses)


def nvmem_8mm(auth):
    fuses = [0] * (64 * WORDS_PER_BANK)
    # locks, bank 1 and bank 2 as read on an open Verdin iMX8M Mini
    fuses[0:3] = [0x02a1a00b, 0x11111111, 0x22222222]               # fake UID
    fuses[4:8] = [0x60000280, 0x00020000, 0x00000000, 0x180022d6]
    fuses[8:12] = [0x01000010, 0x00000000, 0x00000001, 0x00000000]
    fuses[36:39] = [0x02030405, 0x0a0b0001, 0x00000000]             # fake MAC
    if auth:
        fuses[0] |= 0x00000600                                       # SRK_LOCK, SJC_RESP_LOCK
        fuses[7] |= 0x02000000                                       # SEC_CONFIG
        fuses[24:32] = [0xa5a5a5a5] * 8                              # SRK hash
        fuses[7] |= 0x00400000 | 0x00100000 | 0x04000000             # JTAG_SMODE=01, KTE, HEO
        fuses[32:34] = [0xbadabada, 0xbadabada]                      # SJC_RESP, read-locked
    return words_to_bytes(fuses)


def scenario_8mm(outdir, name, auth):
    d = os.path.join(outdir, name)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "nvmem.bin"), "wb").write(nvmem_8mm(auth))


def nvmem_7d(closed):
    fuses = [0] * (16 * WORDS_PER_BANK)
    # locks, banks 1-3 as read on an open Colibri iMX7D
    fuses[0:4] = [0xa0030103, 0x11111111, 0x22222222, 0]             # fake UID
    fuses[4:8] = [0x20000240, 0x10020000, 0x00000000, 0x100028e4]
    fuses[8:12] = [0, 0, 0x00000002, 0]
    fuses[12:16] = [0x4b000000, 0xef000000, 0x000003db, 0x000dd000]
    for b in (4, 5, 10, 11, 15):                                     # read-locked
        fuses[b * 4:b * 4 + 4] = [0xbadabada] * 4
    fuses[36:39] = [0x02030405, 0x0a0b0001, 0x10111213]             # fake MACs
    if closed:
        fuses[0] |= 0x00000208                                       # SRK_LOCK, BOOT_CFG_LOCK OP
        fuses[7] |= 0x02000000 | 0x00C00000 | 0x00100000             # SEC_CONFIG, no debug, KTE
        fuses[24:32] = [0xa5a5a5a5] * 8                              # SRK hash
    return words_to_bytes(fuses)


# HAB store 0 (0x9049c0) as read from Linux on an open Colibri iMX7D,
# up to the last event record: 1x INV_CSF, 4x INV_ASSERTION
HAB_STORE_7D = [
    0x4214000c, 0x00000033, 0x000000ee, 0, 0, 0x42080009, 0x000c6c3f, 0xe1200003,
    0x000000ff, 0x00000005, 0, 0, 0, 0, 0, 0x42500000,
    0x0000001e, 0x00000001, 0, 0, 0x42140000, 0x80d6bffb, 0, 0x00000020,
    0x44107d00, 0xe2280003, 0x0000001d, 0x00000004, 0, 0, 0, 0,
    0x00000001, 0x00000002, 0x00102000, 0x420c0005, 0x00000004, 0, 0x040a000a, 0x53937142,
    0xada000d0, 0x420800db, 0x00cf1133, 0x421400db, 0x00a00c33, 0, 0x00f47f87, 0x20000000,
    0x421400db, 0x00a00c33, 0, 0x2cf47f87, 0xb4010000, 0x421400db, 0x00a00c33, 0,
    0x20f47f87, 0x01000000, 0x421400db, 0x00a00c33, 0, 0x00008087, 0x04000000,
]


def scenario_7d(outdir, name, closed):
    d = os.path.join(outdir, name)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "nvmem.bin"), "wb").write(nvmem_7d(closed))

    ocram = bytearray(0x2000)
    store = list(HAB_STORE_7D)
    if closed:
        first_event = 0xa4 // 4
        store = store[:first_event] + [0] * (len(store) - first_event)
    put(ocram, 0x9c0, store)
    open(os.path.join(d, "ocram-hab.bin"), "wb").write(ocram)
    boot = bytearray(0x20000)
    put(boot, 0x90c4, [0x00020200])  # ROM reports MMC instance 2 (MMC3)
    put(boot, 0x9168, [
        0x10000000, 0x22000000, 0x30000000, 0x41000000,
        0x50000000, 0x61000000, 0x80000000, 0x70,
        0x81000000, 0x9eb, 0x90000000, 0,
        0x91000000, 0x9eb, 0x90000000, 8,
        0x91000000, 0xaf0, 0xa0000000,
        0xf0 if closed else 0x00cf1133, 0xaf7,
        0xc0000000, 0x87800000, 0xafe,
    ])
    open(os.path.join(d, "rom-log-ptr.bin"), "wb").write(words_to_bytes([0x00909168]))
    open(os.path.join(d, "ocram-boot.bin"), "wb").write(boot)
    open(os.path.join(d, "rom-sw-info.bin"), "wb").write(words_to_bytes([0x009090c4]))


    src = bytearray(0x100)
    put(src, 0x58, [0x000028e4, 0])
    put(src, 0x70, [0x00000012 if closed else 0x00000011])
    open(os.path.join(d, "src.bin"), "wb").write(src)

    snvs = bytearray(0x100)
    put(snvs, 0x14, [0x80009d00 if closed else 0x80009b00])
    open(os.path.join(d, "snvs-hp.bin"), "wb").write(snvs)


# HAB persistent area as read with 'md.l 0x90d040' in U-Boot
HAB_AREA = [
    0x4520000c, 0x00000033, 0x000000ee, 0xca6a9e29, 0, 0, 0, 0,
    0x45080009, 0x00cb8c3f, 0xe1500003, 0x000000ff, 0x00000008, 0x71cdea0d, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0x45900000, 0, 0, 0, 0, 0,
    0, 0, 0x45200000, 0, 0, 0, 0, 0,
    0, 0, 0x45500000, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0x450c0005, 0x00000004, 0, 0xe0010702, 0x450800db, 0x00cf1133,
    0x451400db, 0x00a00c33, 0, 0xc0ff9100, 0x20000000, 0x5453788d, 0x451400db, 0x00a00c33,
    0, 0xe0ff9100, 0x0c000000, 0xd6122164, 0x451400db, 0x00a00c33, 0, 0x00009200,
    0x04000000, 0x692808fc, 0x45981000, 0xe30bfa4b,
]

ROM_LOG = [
    0xe0000000, 0x22000000, 0x31000000, 0x41000000, 0x61000000, 0x82000000, 0x00000ee9,
    0x83000000, 0x80000000, 0x00000ef3, 0x81000000, 0x0000d149, 0x50000000, 0x90000000,
    0x00000000, 0x91000000, 0x0000d154, 0x90000000, 0x00000008, 0x91000000, 0x0000d9a1,
    0xa0000000, 0x00cf1133, 0x0000dab2, 0xc0000000, 0x00920000, 0x0000edc2, 0x00000000,
]


def scenario(outdir, name, closed=False, events=True, fallback=False, clobbered=False, debug=None,
             nolog=False):
    d = os.path.join(outdir, name)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "nvmem.bin"), "wb").write(nvmem(closed, debug))

    rom = bytearray(0x1000)
    put(rom, 0x900, [0x457000dd, 0, 0x12b4, 0, 0x11b8, 0, 0x0f68, 0])
    put(rom, 0x9e0, [0 if nolog else 0x009112f8])
    open(os.path.join(d, "rom-low.bin"), "wb").write(rom)

    ocram = bytearray(0x40000)
    area = list(HAB_AREA)
    if not events:
        area = area[:0x118 // 4] + [0] * (len(area) - 0x118 // 4)
    if not clobbered:
        put(ocram, 0xd040, area)
    log = list(ROM_LOG)
    if fallback:
        log[12] = 0x51000000
    put(ocram, 0x112f8, log)
    open(os.path.join(d, "ocram-low.bin"), "wb").write(ocram)

    src = bytearray(0x100)
    put(src, 0x58, [0x00002000, 0x00000001 if not fallback else 0x00000010])
    put(src, 0x70, [0x00040019])
    open(os.path.join(d, "src.bin"), "wb").write(src)

    snvs = bytearray(0x100)
    put(snvs, 0x14, [0x80009d00 if closed else 0x80009b00])
    open(os.path.join(d, "snvs-hp.bin"), "wb").write(snvs)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: mkfixtures.py <outdir>")
    out = sys.argv[1]
    scenario(out, "open")
    scenario(out, "closed", closed=True, events=False)
    scenario(out, "fallback", fallback=True)
    scenario(out, "clobbered", clobbered=True)
    scenario(out, "nolog", nolog=True)
    scenario(out, "hardened", closed=True, events=False, debug="no-debug")
    scenario(out, "jtagdis", closed=True, events=False, debug="jtag-disabled")
    scenario(out, "authjtag", closed=True, events=False, debug="authenticated")
    scenario(out, "weakdebug", closed=True, events=False, debug="no-debug-nokte")
    scenario_8mm(out, "mm-open", auth=False)
    scenario_8mm(out, "mm-auth", auth=True)
    scenario_7d(out, "mx7-open", closed=False)
    scenario_7d(out, "mx7-closed", closed=True)

    # imx-config.fuse as meta-toradex-security writes it (template + values)
    srk = "".join("H:F:%d:%d:0xA5A5A5A5\n" % (6 + i // 4, i % 4) for i in range(8))
    open(os.path.join(out, "imx-config-nodebug.fuse"), "w").write(
        "H:T:HAB\n" + srk + "H:C:1:3:0x02000000\n"
        "# Secure JTAG mode is defeatured on this SoC (ERR052318)\n"
        "H:T:SJC\n"
        "SJC:JTAG_SMODE_NODEBUG:1:3:0x00C00000\n"
        "SJC:KTE:1:3:0x00100000\n"
        "SJC:BOOT_CFG_LOCK_OP:0:0:0x00000008\n")
    open(os.path.join(out, "imx-config-auth.fuse"), "w").write(
        "H:T:SJC\n"
        "SJC:SJC_RESP_LO:8:0:0x445566aa\n"
        "SJC:SJC_RESP_HI:8:1:0x00112233\n"
        "SJC:SJC_RESP_LOCK:0:0:0x00000400\n"
        "SJC:JTAG_SMODE_SECURE:1:3:0x00400000\n"
        "SJC:JTAG_HEO:1:3:0x04000000\n")


main()
