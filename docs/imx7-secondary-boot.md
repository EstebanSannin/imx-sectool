# i.MX7 secondary boot and bootloader rollback

The i.MX7 ROM does not automatically switch between eMMC `boot0` and
`boot1` when the selected bootloader is invalid. Its secondary-boot
mechanism selects another image **inside the same boot partition**.
A bootloader update design must account for this distinction.
[NXP explains this limitation](https://community.nxp.com/t5/i-MX-Processors/Use-boot0-and-boot1-for-primary-and-secondary-bootloader/td-p/919327).
Do not assume the behavior is the same on other i.MX families.

## Two different selections

| Selection | Meaning |
|---|---|
| eMMC `PARTITION_CONFIG`, byte 179 | Selects the hardware boot partition: Boot Partition 1 is Linux `boot0`; Boot Partition 2 is Linux `boot1` |
| ROM primary / secondary | Selects an image offset within that partition |

The Secondary Image Table (SIT) contains an offset, not a partition number.
It cannot point from `boot1` into `boot0`. The current EXT_CSD value describes
configuration; it is not a record of what the ROM actually booted.

## Image layout

The tested layout uses these offsets from the start of the selected
boot partition. Image sizes must leave space for the environment and any
other reserved data.

| Item | Byte offset |
|---|---|
| SIT, 20 bytes | `0x200` |
| Primary IVT | `0x400` |
| Secondary IVT in this example | `0x100400` |

The SIT is five little-endian 32-bit words:

```
0, 0, 0x00112233, 0x00000800, 0
```

`0x800` is the secondary area offset in 512-byte sectors. The IVT is at
`0x800 * 512 + 0x400`. The extra `0x400` matters.
See [U-Boot's redundant-boot documentation](https://docs.u-boot.org/en/latest/board/nxp/psb.html).

When the ROM rejects the primary image, it can set
`SRC_GPR10[30]` (`PERSIST_SECONDARY_BOOT`) and perform a warm reset.
It then uses the SIT to find the secondary image. Software can also force
this selection. A cold reset clears the bit.

This covers failures detected by the ROM. It does not detect a bootloader
that starts and later hangs, or an OS that fails to become healthy. Those
cases need a watchdog, attempt limit and boot-success policy.

## DDR initialization requirement

A warm reset can leave the i.MX7 DDR controller in a state that prevents
DCD initialization from working again. Our unmodified image failed on
both primary and forced-secondary warm boots.

[NXP's workaround](https://community.nxp.com/t5/i-MX-Security/BootROM-HABv4-Redundant-boot-support-is-not-triggered-when-first/ta-p/1119740)
adds these writes before the normal DDR setup:

```
DATA 4 0x30340020 0x00000000
DATA 4 0x30391000 0x00000003
```

Apply this in the image build and validate it with the board's DDR setup.
For a closed device, sign the final image including the updated DCD.
The experiment changed an unsigned test image; it is not a production
image-patching procedure.

## What was tested

On an open Colibri iMX7Dual with U-Boot `2024.07-7.7.0`:

- The secondary booted after adding the DDR workaround to its DCD.
- Invalidating only the primary IVT tag, clearing forced selection and
  resetting caused the ROM to boot the secondary automatically.
- The ROM log recorded `0x51` and a jump to the bootloader entry point.
- Restoring the primary tag restored normal primary boot (`0x50`).

Both images were in `boot0`. No hardware-partition switch was required.
This does not validate authentication-failure recovery on a closed device.
The detailed captures are summarized in [the boot-log investigation](imx7-boot-log.md).

## Proposed recovery image

A small secondary image could provide the missing rollback step. This is
not implemented yet. A possible design is:

1. Reserve a SIT and recovery image in both hardware boot partitions.
2. Before activating an update, verify the new primary and its recovery
   image. Preserve the other partition as the known-good bootloader.
3. If the ROM rejects the new primary, let it start the recovery image
   in that same partition.
4. Have recovery read durable update metadata, select the known-good
   partition through eMMC `PARTITION_CONFIG`, and verify the write.
5. Clear the secondary-selection bit and reset with a validated reset
   method, so the ROM starts the primary image in the rollback partition.

Recovery must preserve unrelated EXT_CSD bits and identify the known-good
partition explicitly. Blindly toggling partitions can create a reboot loop.
If no valid rollback target exists, recovery needs a defined service mode.

The recovery image must remain bootable if the primary update is interrupted.
The updater must preserve its space and the SIT, including when erasing or
writing a complete boot partition. Update metadata also needs a power-loss
safe format and a bounded retry policy. On closed devices, every recovery
and rollback image must satisfy HAB and the product's anti-rollback policy.

## Next implementation and tests

Start by defining the image layout and update metadata together with the
bootloader updater. Then implement recovery using the smallest practical
loader with eMMC support and the required DDR workaround.

Validate both partition directions, interrupted updates and metadata writes,
invalid primary headers, authentication failures on a closed device,
watchdog resets, and an unusable rollback target. Confirm normal boot and
recovery with serial access before testing unattended updates.

`imx-sectool` reports the ROM image selection. It must not translate
`SECONDARY` into `boot1`, or claim that the marker alone proves an automatic
fallback: software can request that selection too.
