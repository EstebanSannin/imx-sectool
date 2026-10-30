#!/bin/sh
#
# run.sh -- host tests: run imx-sectool against synthetic fixtures and compare
# stdout and exit code with the golden files in test/expected/.
#
# usage: test/run.sh <path-to-imx-sectool> [--update]
#   --update rewrites the golden files from the current output (review the
#   diff before committing!).
#
# License: MIT.  Author: Stefano Viola <stefano.viola@toradex.com>

set -u
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
UPDATE=${2:-}
T=$(cd "$(dirname "$0")" && pwd)
OUT=$T/out
FIX=$OUT/fixtures

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$FIX"
python3 -I "$T/mkfixtures.py" "$FIX" || exit 1
ln -sf "$BIN" "$OUT/bin/fuse"
ln -sf "$BIN" "$OUT/bin/hab_status"

pass=0
fail=0
SOC=i.MX8MP

# case <name> <scenario> <expected-exit> <command...>
case_() {
    name=$1 scen=$2 want=$3
    shift 3
    got_out=$OUT/$name.out
    IMX_SECTOOL_SOC=$SOC IMX_SECTOOL_NVMEM=$FIX/$scen/nvmem.bin \
        IMX_SECTOOL_MEM_DUMP=$FIX/$scen PATH="$OUT/bin:$PATH" \
        "$@" > "$got_out" 2> "$OUT/$name.err" < /dev/null
    rc=$?
    exp=$T/expected/$name.txt
    if [ "$UPDATE" = "--update" ]; then
        cp "$got_out" "$exp"
    fi
    if [ "$rc" != "$want" ]; then
        echo "FAIL $name: exit $rc, expected $want"; cat "$OUT/$name.err"; fail=$((fail + 1)); return
    fi
    if ! diff -u "$exp" "$got_out" > "$OUT/$name.diff" 2>&1; then
        echo "FAIL $name: output differs"; cat "$OUT/$name.diff"; fail=$((fail + 1)); return
    fi
    echo "ok   $name"
    pass=$((pass + 1))
}

# fuse: U-Boot syntax, output and exit codes
case_ fuse-read-bank1        open 0 fuse read 1 0 4
case_ fuse-read-q            open 0 fuse read -q 1 0 4
case_ fuse-read-locked       open 1 fuse read 4 0 4
case_ fuse-read-invalid      open 1 fuse read 96 0
case_ fuse-cmp-pass          open 0 fuse cmp 1 3 0x98002000
case_ fuse-cmp-fail          open 1 fuse cmp 1 3 0x98000000
case_ fuse-readv             open 0 fuse readv 1 0 cfg 4
case_ fuse-readv-q           open 0 fuse readv -q 1 3 cfg
case_ fuse-readm             open 0 fuse readm 1 2 0x40000000 2
case_ fuse-usage             open 2 fuse read
case_ fuse-sense             open 3 fuse sense 1 3
case_ fuse-prog-noop         open 0 fuse prog -y 1 3 0x98002000
case_ fuse-prog-abort        open 1 fuse prog 6 0 0x12345678
case_ fuse-prog-locked       open 1 fuse prog -y 4 0 0x1

# hab_status: U-Boot output, meta-toradex-security exit codes
case_ hab-open-events        open      1 hab_status
case_ hab-closed-clean       closed    0 hab_status
case_ hab-clobbered          clobbered 4 hab_status

# boot information
case_ romlog-open            open      0 "$BIN" romlog
case_ romlog-fallback        fallback  0 "$BIN" romlog
case_ romlog-json            open      0 "$BIN" --json romlog
case_ reset-cause-por        open      5 "$BIN" reset-cause
case_ reset-cause-json       fallback  5 "$BIN" reset-cause --json

# debug-status: secure debug as provisioned by meta-toradex-security
case_ debug-open             open      1 "$BIN" debug-status
case_ debug-nodebug          hardened  0 "$BIN" debug-status
case_ debug-nodebug-expect   hardened  0 "$BIN" debug-status --expect no-debug
case_ debug-expect-mismatch  hardened  1 "$BIN" debug-status --expect jtag-disabled
case_ debug-expect-open      open      0 "$BIN" debug-status --expect open
case_ debug-jtagdis          jtagdis   0 "$BIN" debug-status
case_ debug-auth-8mp         authjtag  1 "$BIN" debug-status
case_ debug-weak             weakdebug 5 "$BIN" debug-status
case_ debug-json             hardened  0 "$BIN" --json debug-status
case_ debug-profile          hardened  0 "$BIN" debug-status --profile "$FIX/imx-config-nodebug.fuse"
case_ debug-profile-auth     authjtag  1 "$BIN" debug-status --profile "$FIX/imx-config-auth.fuse"
case_ debug-bad-expect       hardened  2 "$BIN" debug-status --expect maybe

# audit
case_ audit-open             open      1 "$BIN" audit
case_ audit-hardened         hardened  5 "$BIN" audit
case_ audit-profile          hardened  5 "$BIN" audit --profile "$FIX/imx-config-nodebug.fuse"
case_ audit-profile-mismatch open      1 "$BIN" audit --profile "$FIX/imx-config-nodebug.fuse"
case_ audit-json             hardened  5 "$BIN" audit --json
case_ audit-clobbered        clobbered 1 "$BIN" audit

# fuse by field name (imx-sectool extension)
case_ fuse-name-read         open      0 fuse read JTAG_SMODE
case_ fuse-name-read-nobr    open      0 fuse read sec_config1
case_ fuse-name-readv        open      0 fuse readv -q SPEED_GRADING sg
case_ fuse-name-cmp          open      0 fuse cmp TEMP_GRADE 2
case_ fuse-name-cmp-fail     open      1 fuse cmp TEMP_GRADE 3
case_ fuse-name-toobig       open      2 fuse prog -y KTE 2
case_ fuse-name-conflict     open      1 fuse prog -y BOOT_CFG_LOCK 1
case_ fuse-name-noop         open      0 fuse prog -y BT_FUSE_SEL 1
case_ fuse-name-abort        open      1 fuse prog KTE 1
case_ fuse-name-unknown      open      2 fuse read NOT_A_FUSE

# fusemap
case_ fusemap-all            open      0 "$BIN" fusemap
case_ fusemap-field          open      0 "$BIN" fusemap SPEED_GRADING
case_ fusemap-reg            open      0 "$BIN" fusemap OCOTP_BOOT_CFG0
case_ fusemap-reg-nofields   open      0 "$BIN" fusemap OCOTP_TESTER2
case_ fusemap-mac0           open      0 "$BIN" fusemap MAC0
case_ fusemap-mac1           open      0 "$BIN" fusemap mac1
case_ fusemap-srk            hardened  0 "$BIN" fusemap SRK_HASH
case_ fusemap-json           hardened  0 "$BIN" --json fusemap OCOTP_LOCK
case_ fusemap-unknown        open      2 "$BIN" fusemap FOO

# bootinfo
case_ bootinfo-open          open      0 "$BIN" bootinfo
case_ bootinfo-fallback      fallback  0 "$BIN" bootinfo
case_ bootinfo-json          open      0 "$BIN" --json bootinfo
case_ bootinfo-nolog         nolog     3 "$BIN" bootinfo
case_ romlog-nolog           nolog     4 "$BIN" romlog

# i.MX8M Mini: fuses only, no /dev/mem regions yet
SOC=i.MX8MM
case_ mm-fuse-read           mm-open   0 fuse read 1 0 4
case_ mm-fuse-read-invalid   mm-open   1 fuse read 64 0
case_ mm-fusemap-all         mm-open   0 "$BIN" fusemap
case_ mm-fusemap-srk         mm-auth   0 "$BIN" fusemap SRK_HASH
case_ mm-debug-open          mm-open   1 "$BIN" debug-status
case_ mm-debug-auth          mm-auth   0 "$BIN" debug-status --expect authenticated
case_ mm-hab-status          mm-auth   3 hab_status
case_ mm-bootinfo            mm-open   3 "$BIN" bootinfo
case_ mm-audit               mm-auth   3 "$BIN" audit

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
