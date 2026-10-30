#!/bin/sh
#
# phase0.sh -- read-only discovery for imx-sectool (DESIGN.md, section 10).
#
# Collects what Linux exposes about fuses, boot and security on an i.MX8M Plus
# into one output directory. Nothing is written to the hardware: fuses are read
# through nvmem, and physical memory is only read through the whitelisted
# 'memdump' helper.
#
# Two stages, so the risky part can be judged before it runs:
#   ./phase0.sh            sysfs/procfs/nvmem only (no /dev/mem access)
#   MMIO=1 ./phase0.sh     additionally dumps the memdump regions; SNVS is
#                          skipped while its clock looks gated unless FORCE=1
#
# The output contains device identifiers (UID, fuse values): keep it local,
# never commit it.
#
# License: MIT.  Author: Stefano Viola <stefano.viola@toradex.com>

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
MEMDUMP=${MEMDUMP:-$HERE/memdump}
OUT=${OUT:-$PWD/phase0-out-$(date +%Y%m%d-%H%M%S)}
MMIO=${MMIO:-0}
FORCE=${FORCE:-0}

mkdir -p "$OUT" || exit 1
LOG="$OUT/summary.txt"

say() { echo "$*" | tee -a "$LOG"; }
section() { say ""; say "==== $* ===="; }

# run a command, store its output in $OUT/<file>, never abort the script
grab() {
    f=$1; shift
    "$@" > "$OUT/$f" 2>&1
    rc=$?
    [ $rc -eq 0 ] || say "note: '$*' exited $rc (see $f)"
}

if [ "$(id -u)" != "0" ]; then
    echo "error: run as root (nvmem, /proc/iomem and /dev/mem need it)" >&2
    exit 1
fi

# ---- system ---------------------------------------------------------------
section "system"
grab uname.txt uname -a
grab os-release.txt cat /etc/os-release
grab cmdline.txt cat /proc/cmdline
for a in machine family soc_id revision serial_number; do
    [ -r /sys/devices/soc0/$a ] && say "soc0/$a: $(cat /sys/devices/soc0/$a)"
done
SOC=$(cat /sys/devices/soc0/soc_id 2>/dev/null)
[ "$SOC" = "i.MX8MP" ] || say "warning: soc_id is '$SOC', the helper tables target i.MX8MP"

# ---- kernel config ----------------------------------------------------------
section "kernel config"
if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz > "$OUT/kconfig.txt"
elif [ -r "/boot/config-$(uname -r)" ]; then
    cp "/boot/config-$(uname -r)" "$OUT/kconfig.txt"
fi
if [ -s "$OUT/kconfig.txt" ]; then
    grep -E '^(# )?CONFIG_(DEVMEM|STRICT_DEVMEM|IO_STRICT_DEVMEM|NVMEM|NVMEM_IMX_OCOTP|NVMEM_SYSFS|SRAM|IMX2_WDT|CRYPTO_DEV_FSL_CAAM|TEE|OPTEE|IMX_SEC_ENCLAVE|IMX_EL_ENCLAVE|DEBUG_FS)[ =]' \
        "$OUT/kconfig.txt" | tee -a "$LOG"
else
    say "kernel config not available (/proc/config.gz missing)"
fi
[ -e /dev/mem ] && say "/dev/mem: present" || say "/dev/mem: MISSING"

# ---- fuses via nvmem ------------------------------------------------------
section "nvmem"
ls -l /sys/bus/nvmem/devices/ 2>&1 | tee -a "$LOG"
for d in /sys/bus/nvmem/devices/*; do
    [ -r "$d/nvmem" ] || continue
    n=$(basename "$d")
    if dd if="$d/nvmem" of="$OUT/nvmem-$n.bin" bs=4 2>/dev/null; then
        say "$n: $(wc -c < "$OUT/nvmem-$n.bin") bytes dumped"
    else
        say "$n: read failed"
    fi
done
# U-Boot-style view of the OCOTP dump: 4 words per bank on i.MX8M
OCOTP=$(ls "$OUT"/nvmem-imx-ocotp*.bin 2>/dev/null | head -n 1)
if [ -n "$OCOTP" ]; then
    hexdump -v -e '4/4 "%08x " "\n"' "$OCOTP" |
        awk '{ printf "bank %2d: %s\n", NR - 1, $0 }' > "$OUT/fuses-by-bank.txt"
    say "fuses-by-bank.txt written (compare with U-Boot 'fuse read <bank> 0 4')"
    grep -ci badabada "$OUT/fuses-by-bank.txt" | sed 's/^/read-locked words (0xBADABADA count, lines): /' | tee -a "$LOG"
fi

# ---- memory map, DT, clocks -------------------------------------------------
section "memory map and device tree"
grab iomem.txt cat /proc/iomem
grep -iE 'sram|ocram|900000|3037|3039|3035' "$OUT/iomem.txt" | tee -a "$LOG"
for n in /proc/device-tree/soc@0/sram@* /proc/device-tree/sram@* /proc/device-tree/reserved-memory; do
    [ -e "$n" ] && say "dt: $n present" && ls "$n" >> "$OUT/dt-nodes.txt" 2>&1
done

CLK=/sys/kernel/debug/clk/clk_summary
if [ -r "$CLK" ]; then
    grep -iE 'ocotp|snvs|ocram|src' "$CLK" | tee "$OUT/clocks.txt" | tee -a "$LOG"
else
    say "clk_summary not readable (debugfs not mounted?) - clock gating unknown"
fi

# ---- boot/firmware hints ----------------------------------------------------
section "firmware and watchdog"
for w in /sys/class/watchdog/watchdog*; do
    [ -r "$w/bootstatus" ] && say "$(basename "$w") bootstatus: $(cat "$w/bootstatus")"
done
dmesg 2>/dev/null | grep -iE 'optee|tee:|bl31|tf-a|hab|caam|ocotp|snvs|imx8mp|soc revision' > "$OUT/dmesg-filtered.txt"
say "dmesg-filtered.txt: $(wc -l < "$OUT/dmesg-filtered.txt") lines"

# ---- MMIO stage ---------------------------------------------------------------
section "mmio"
if [ "$MMIO" != "1" ]; then
    say "skipped (re-run with MMIO=1 after reviewing clocks and iomem)"
elif [ ! -x "$MEMDUMP" ]; then
    say "skipped: memdump helper not found at $MEMDUMP"
else
    for r in rom-low ocram-low src; do
        if "$MEMDUMP" dump "$r" "$OUT/mem-$r.bin" 2>>"$LOG"; then
            say "$r: dumped"
        else
            say "$r: FAILED"
        fi
    done
    snvs_gated=0
    [ -r "$CLK" ] && grep -iE '^ *snvs' "$CLK" | awk '{ exit ($2 > 0) }' && snvs_gated=1
    if [ $snvs_gated -eq 1 ] && [ "$FORCE" != "1" ]; then
        say "snvs-hp: skipped, clock enable count is 0 (FORCE=1 to override)"
    elif "$MEMDUMP" dump snvs-hp "$OUT/mem-snvs-hp.bin" 2>>"$LOG"; then
        say "snvs-hp: dumped"
    else
        say "snvs-hp: FAILED"
    fi
    # quick look at the ROM log pointer (rom-low + 0x9e0)
    if [ -s "$OUT/mem-rom-low.bin" ]; then
        ptr=$(hexdump -s 0x9e0 -n 4 -e '1/4 "%08x"' "$OUT/mem-rom-low.bin")
        say "ROM log pointer @0x9e0: 0x$ptr (valid range 0x00900000-0x00b00000)"
    fi
fi

section "done"
say "output: $OUT"
say "reminder: contains device identifiers, keep it local"
