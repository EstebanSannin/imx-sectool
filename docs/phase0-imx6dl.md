# i.MX6DualLite support

Tested on an open Colibri iMX6DL 512MB module with Torizon OS Upstream
7.7.0, Linux 6.6.143 and U-Boot 2024.07. The NXP chip revision was 1.2;
this is the silicon revision, not the Toradex module revision.

## What works

- Fuse reads through `imx-ocotp` nvmem, including named fields and SRK hash.
- `debug-status` and the fuse checks in `audit`.
- SNVS security state and SRC boot configuration.
- `bootinfo` decodes the configured controller. `MMC3` means USDHC3,
  not Linux `mmcblk3` and not an eMMC boot partition.

Linux fuse reads matched the selected U-Boot reads. The board was open,
with JTAG enabled, an empty SRK hash, industrial temperature grade and
792 MHz speed grade. No fuses were programmed. Closed-device and secure
JTAG cases have fixture coverage but have not been tested on hardware.

## HAB and ROM logs

The ROM event reader points to a store table at `0x00010240`. Store 0 is
`0x00904000–0x00904b7f`. Its HAB 4.1 records use 4-byte alignment.

Before Linux starts, that store contains four events: one invalid-address
report and three invalid-assertion reports. The reader reproduces those
records from a captured fixture. This U-Boot build has no `hab_status`
command, so comparison with its command output remains pending.

The format-0 ROM log pointer is at `0x000000d8` on the tested chip and
points to `0x00901ab8`. The log records authentication and the jump to SPL.
The tool checks the ROM vector-table header before choosing the pointer
location. The older table placement at `0x94` has fixture coverage only.

**Both buffers are lost by the time the tested Linux image reaches its
shell.** They are present at the U-Boot prompt and zeroed afterward. The
low OCRAM area also contains Linux power-management data. The exact code
that clears each buffer has not been traced.

Consequences:

- `hab_status` still reports the fuse configuration and SNVS state, but
  returns 4 when the expected HAB record is missing. It never reports an
  empty event list from a zeroed store.
- `romlog` prints raw format-0 events when preserved and returns 4 for an
  empty or invalid buffer.
- `bootinfo` reports the available fuse and SRC information, returning 3
  when the ROM log cannot be read. Authentication and entry address appear
  only when the log passes validation.
- The configured boot device is not proof of the actual ROM boot path.
  No primary/secondary image or eMMC partition result is inferred.

A userspace reader cannot recover data that has been overwritten. Keeping
these logs on this image would require preserving the memory or exporting
a copy before Linux reuses it. This change adds neither a device-tree
requirement nor a bootloader or kernel modification.

## Reset cause

SRC_SRSR is zero in Linux after both a cold boot and a reboot. U-Boot
clears it after reading it. `reset-cause` reports unknown and returns 3.
The i.MX6 decoder uses its own offsets and reset bits, including temperature
reset at bit 8 and warm boot at bit 16.

## Layout differences

| Item | i.MX6DL |
|---|---|
| nvmem | 512 bytes; 16 banks of 8 words |
| Secure boot | `SEC_CONFIG`, bank 0 word 6 bit 1 |
| JTAG policy | Bank 0 word 6 |
| SJC response | Bank 4 words 0–1 |
| SRK hash | Bank 3 words 0–7 |
| SRK lock | Bank 0 word 0 bit 14 |
| SRK revocation | Bank 5 word 7 bits 2:0 |
| MAC | One address, bank 4 words 2–3 |
| SRC | Base `0x020d8000`; SBMR1 `+0x04`, SRSR `+0x08`, SBMR2 `+0x1c` |
| SNVS HPSR | `0x020cc014` |

There is no verified `SDP_DISABLE` field in this descriptor. The audit
reports that check as unavailable rather than applying an i.MX7/8M bit.

## Sources

- [Crucible IMX6DL fusemap](https://github.com/usbarmory/crucible/blob/master/cmd/crucible/fusemaps/IMX6DL.yaml).
- U-Boot: `arch/arm/mach-imx/mx6/soc.c`, `mac.c`, `cpu.c` and
  `arch/arm/include/asm/arch-mx6/imx-regs.h`.
- [NXP AN4581, appendix B](https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/imx-processors/194321/1/AN4581_2018.pdf): SRK revocation.
- [U-Boot i.MX6 ROM-log reader proposal](https://www.mail-archive.com/u-boot@lists.u-boot-project.org/msg02440.html): format-0 parameter counts.
- Read-only ROM, OCRAM, SRC and SNVS captures from the test board.
