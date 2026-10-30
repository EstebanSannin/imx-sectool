# Phase 0 findings: i.MX8M Plus

Read-only tests on a Verdin iMX8M Plus (SoC revision 1.1) on a Dahlia
carrier board, with a Torizon OS 8 development build (Linux 6.18). Tools:
`tools/phase0/phase0.sh` and `tools/phase0/memdump`. Device identifiers
(UID, serial, network) are not included here.

## Kernel

| Item | Value | Result |
|---|---|---|
| `CONFIG_DEVMEM` | y | `/dev/mem` is present |
| `CONFIG_STRICT_DEVMEM` | y | only non-RAM ranges can be mapped; ROM, OCRAM, SRC and SNVS all work |
| `CONFIG_IO_STRICT_DEVMEM` | not set | ranges used by drivers can still be mapped |
| `CONFIG_NVMEM_IMX_OCOTP` | y | `imx-ocotp0`, 384 words |
| `/proc/iomem` | no entry for OCRAM, SRC or SNVS | no driver uses them |
| OCOTP clock | off when idle | never access OCOTP registers directly; use nvmem |
| SNVS and OCRAM clocks | not in the Linux clock tree | Linux cannot turn them off; reads are safe |

All `memdump` regions were read with no errors and no kernel messages.

## Fuses (nvmem)

- 4 words per bank, `offset = (bank * 4 + word) * 4`.
- Read-locked words (for example banks 4 and 5) read `0xBADABADA`.
- Test board: HAB open (`SEC_CONFIG` clear), SRK hash all zero,
  `JTAG_SMODE = 00`, `SJC_DISABLE` and `KTE` clear, so secure debug is not
  provisioned. This matches the SNVS and HAB data below.

## Boot ROM

| Offset | Content |
|---|---|
| `0x900` | HAB ROM vector table, header `0x457000dd` (HAB 4.5) |
| `0x980` | ROM API table |
| `0x9e0` | ROM log pointer: `0x009112f8`, in OCRAM |

## ROM event log

Decoded with the AN12853 event IDs used by U-Boot:

```
e0000000  log header
22000000  secure config
31000000  boot device
41000000  strap/fuse
61000000  ...
82000000  device pre-config  ts 0x00000ee7
80000000  device init start  ts 0x00000ef0
81000000  device init done   ts 0x0000f95b
50000000  primary image      <- no 0x51: no fallback to the secondary image
90000000  read start         0x00000000
91000000  read done          ts 0x0000f967
90000000  read start         0x00000008
91000000  read done          ts 0x000101b2
a0000000  auth result        0x00cf1133 ts 0x000102c2   <- HAB failure, CSF
c0000000  jump               0x00920000 ts 0x000115d2   <- SPL in OCRAM
00000000  end
```

To do: decode the parameter bits of events `0x2x`, `0x3x`, `0x4x`, `0x6x`
and the timestamp unit.

## HAB event area

- `EVT_DEF` record (tag `0x0C`, length `0x20`, version `0x45`) at
  `0x0090d040`.
- Events (tag `0xDB`) from `0x0090d158`:

| Event | Status | Reason | Context | Data |
|---|---|---|---|---|
| 1 | `0x33` HAB_FAILURE | `0x11` HAB_INV_CSF | `0xCF` HAB_CTX_CSF | — |
| 2 | `0x33` | `0x0C` HAB_INV_ASSERTION | `0xA0` HAB_CTX_ASSERT | `0x0091ffc0`, len `0x20` |
| 3 | `0x33` | `0x0C` | `0xA0` | `0x0091ffe0`, len `0x0c` |
| 4 | `0x33` | `0x0C` | `0xA0` | `0x00920000`, len `0x04` |

These are the normal events for an unsigned image on an open i.MX8M. So
`hab_status` works from Linux without the TF-A call and without a kernel
module, like U-Boot does on the i.MX7ULP M4.

Records with other tags (`0x09`, `0x03`, `0x05`) come between `EVT_DEF` and
the first event; the parser skips them by length.

## SRC

| Register | Value | Meaning |
|---|---|---|
| `SBMR1` +0x58 | `0x00002000` | boot config |
| `SRSR` +0x5c | `0x00000001` | power-on, but see "Reset cause" |
| `SBMR2` +0x70 | `0x00040019` | boot from fuses, `SEC_CONFIG` open |
| `GPR10` +0x98 | `0x00000000` | no secondary boot flag |

## SNVS

| Register | Value | Meaning |
|---|---|---|
| `HPSR` +0x14 | `0x80009b00` | SSM state `0xB`: non-secure, as expected on an open device |
| `HPCOMR` +0x04 | `0x80002100` | — |
| `HPSVSR` +0x18 | `0x00002000` | security violation status (not decoded) |

## Check against U-Boot

On one boot, we stopped autoboot on the serial console, ran reference
commands in U-Boot, then booted Linux and read the same addresses with
`memdump`.

| Check | U-Boot | Linux | Result |
|---|---|---|---|
| `fuse read` banks 0, 1, 6, 7, 8 | words | nvmem | same |
| `fuse read 4 0 4` (read-locked) | `Access protect error` | `0xBADABADA` | the `fuse` clone prints U-Boot's error and fails like U-Boot |
| HAB vector table `0x900` | `457000dd ...` | same | same |
| ROM log pointer and events | `0x009112f8`, timestamps `0xee9`, `0xef3`, ... | same values and timestamps | same: the data is from this boot and reaches Linux unchanged |
| HAB event area | `EVT_DEF` and 4 events | same bytes | same |
| SRC and SNVS registers | `00002000` / `00000001` / `00040019` / `00000000`, `80009b00` | same | same |

Other notes:

- This U-Boot has no `hab_status` command (`Unknown command`). imx-sectool
  shows the HAB status even when the bootloader cannot.
- SPL prints `Boot Stage: Primary boot`, which matches ROM log event `0x50`.
- U-Boot `md.l` prints little-endian words, so the header bytes
  `0c 00 20 45` show as `4520000c`.

## Reset cause

`SRC_SRSR` read from Linux after different resets:

| Reset | `SRSR` | watchdog `bootstatus` |
|---|---|---|
| power cycle | `0x00000001` | 0 |
| `systemctl reboot` | `0x00000001` | 0 |
| `sysrq b` | `0x00000001` | 0 |
| kernel panic with `kernel.panic=5` | `0x00000001` | 0 |
| kernel panic with `kernel.panic=0` (hang) | `0x00000001` | 0 |

The value is always the same. Linux restarts through TF-A (PSCI), which
leaves no trace in `SRSR`. The last test should have shown a watchdog
reset, but the board came back only after about 15 minutes instead of
30 seconds, so it does not show what a watchdog reset records. To check:
why the watchdog did not reset the board, and if `WDOG1_WRSR`
(`0x30280004`) keeps the reset cause.

## Result

1. The ROM log and the HAB events can be read from Linux with no
   device-tree change and no kernel module.
2. `hab_status` on i.MX8M reads the HAB event area at `0x0090d040`.
3. `reset-cause` cannot tell resets apart on this boot flow. It prints the
   value with a warning and exits with the warning code.
4. nvmem is enough for all fuse reads; OCOTP registers are not used.
