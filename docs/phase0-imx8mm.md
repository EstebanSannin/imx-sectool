# Phase 0 findings: i.MX8M Mini

Read-only tests on a Verdin iMX8M Mini Quad on a Dahlia carrier board,
with a Torizon OS 8 development build (Linux 6.18). Tools:
`tools/phase0/memdump`. Device identifiers (UID, serial, network) are
not included here.

The i.MX8M Mini uses the same HABv4 mechanism as the i.MX8M Plus. Only
the OCRAM addresses differ, so `hab_status`, `romlog`, `bootinfo` and
`reset-cause` work the same way once the right addresses are known.

## Kernel

| Item | Value | Result |
|---|---|---|
| `CONFIG_DEVMEM` | y | `/dev/mem` is present |
| `CONFIG_STRICT_DEVMEM` | y | only non-RAM ranges can be mapped; ROM, OCRAM, SRC and SNVS all work |
| `CONFIG_IO_STRICT_DEVMEM` | not set | ranges used by drivers can still be mapped |
| `CONFIG_NVMEM_IMX_OCOTP` | y | `imx-ocotp0`, 256 words |

A plain `read()` of `/dev/mem` (for example `dd`) fails with `EFAULT` on
these ranges; the ROM and OCRAM must be mapped with `mmap`, which is what
`memdump` and `imx-sectool` do.

## Fuses (nvmem)

- 4 words per bank, `offset = (bank * 4 + word) * 4`, like the 8MP.
- Test board: HAB open (`SEC_CONFIG` clear), SRK hash all zero,
  `JTAG_SMODE = 00`, so secure debug is not provisioned.

## Boot ROM

| Offset | Content |
|---|---|
| `0x900` | HAB ROM vector table, header `0x437000dd` (HAB 4.3) |
| `0x9e0` | ROM log pointer: `0x0090e558`, in OCRAM |

## HAB event store

The ROM's HAB persistent area is in OCRAM at `0x908040` (the 8MP uses
`0x90d040`). It starts with an `EVT_DEF` record (tag `0x0C`, version
`0x43`), then the event records (tag `0xDB`), 8-byte aligned.

On the open board the events are the usual set for an unsigned image:

- 1x `HAB_INV_CSF` (`0x33 0x11 0xCF`)
- 3x `HAB_INV_ASSERTION` (`0x33 0x0C 0xA0`)

## ROM event log

At `0x0090e558`, decoded with the event IDs used by U-Boot. The last
entries are the image authentication result `0x00cf1133` (which matches
the first HAB event) and the jump to the boot image at `0x007e1000`.

## SRC and SNVS

| Register | Address | Note |
|---|---|---|
| `SRC_SBMR1` | `0x30390000 + 0x58` | boot config |
| `SRC_SRSR` | `0x30390000 + 0x5c` | reset status; U-Boot clears it before Linux |
| `SRC_SBMR2` | `0x30390000 + 0x70` | BMOD and boot mode |
| `SNVS_HPSR` | `0x30370000 + 0x14` | `0x80009b00` on the open board (non-secure) |

Because U-Boot clears `SRSR`, `reset-cause` cannot tell a power-on from a
reboot, the same as on the 8MP.

## Verified on hardware

`hab_status`, `romlog`, `bootinfo`, `reset-cause` and `audit` were run on
the board and gave the expected output. The events have not yet been
compared against U-Boot `hab_status` on the same boot, and a closed board
has not been tested.
