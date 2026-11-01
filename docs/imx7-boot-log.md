# i.MX7 boot-log investigation

The i.MX7Dual ROM does keep a boot log. The earlier conclusion that it did
not was wrong. `romlog` now decodes it, and `bootinfo` reports its image selection,
authentication result and entry address.

## Read-only findings

Checked on an open i.MX7Dual board with NXP silicon revision 1.3:

| Item | Observed value |
|---|---|
| ROM log pointer | ROM word `0x1e0` points to `0x00909168` |
| Buffer capacity | 64 words, from the saved ROM code |
| Written-word count | `0x009074cc` contains 24, unchanged before and after reading |
| Boot marker | `0x50000000`, primary image |
| Authentication entry | `0xa0000000`, result `0x00cf1133`, then timestamp |
| Image entry | `0xc0000000`, address `0x87800000`, then timestamp |
| Persistent selection | `SRC_GPR10[30] = 0` |

The authentication result matches the first HAB event: failure, invalid
CSF, CSF context. It does not mean the image was successfully authenticated.
The board is open and boots an unsigned image.

The log survived into Linux. Reading its pointer and 256-byte OCRAM buffer
did not reset the board. No boot configuration, image or fuse was changed.

## ROM code evidence

Offline inspection of the saved ROM dump found:

- `0x730a`: appends an event word, up to 64 words.
- `0x732e`: appends an event and one parameter.
- `0x734a`: appends a timestamp.
- `0x6fe2`: checks the persistent selection and logs `0x50` or `0x51`.
- `0x7c40`: reads `SRC_GPR10[30]`; `0x7c4c` sets it.
- `0x68fa`: checks the secondary selection and processes the Secondary
  Image Table, including its `0x00112233` tag.

These code addresses describe the inspected ROM, not a runtime interface.
Use the ROM pointer for the buffer. Do not assume the count address is
stable across chip revisions without further evidence.

The observed events match the shared decoder: `0x90` has one parameter,
while `0xa0` and `0xc0` have two. The tool reads at most 64 words and requires
an end marker. Empty or incomplete logs are reported as unavailable.
The internal written-word counter is not used.

## Bootloader update comparison

After a successful OTA update to U-Boot `2024.07-7.0.0`, the board rebooted
and exported the new version in the device tree. The update service also
reported success.

| Observation | Before | After |
|---|---|---|
| EXT_CSD PARTITION_CONFIG | `0x08`, Boot Partition 1 | `0x50`, Boot Partition 2 |
| ROM image marker | `0x50`, primary | `0x50`, primary |
| SRC_GPR10 selection | primary | primary |
| ROM boot device | MMC3 | MMC3 |
| Authentication result | `0x00cf1133` | `0x00cf1133` |
| Entry address | `0x87800000` | `0x87800000` |

The log remained readable and its timestamps changed after reboot.
The primary/secondary marker did not track the configured hardware partition
switch. It must not be translated into Linux `boot0` or `boot1`.
EXT_CSD still reports configuration, not proof of the partition actually
used by the ROM. This update did not deliberately trigger ROM fallback.

## Secondary-image fallback test

A separate open Colibri iMX7Dual board was tested with serial and USB
recovery available, using U-Boot `2024.07-7.7.0`. Both eMMC boot partitions
were backed up before writing.

The test used two images inside Linux `boot0`:

| Item | Byte offset |
|---|---|
| Secondary Image Table | `0x200` |
| Primary IVT | `0x400` |
| Secondary IVT | `0x100400` |

The table contained tag `0x00112233` and first sector `0x800`.
The secondary initially matched the primary byte for byte.

The first warm-reset test failed before reaching U-Boot. The unchanged
primary also failed with the same reset. Adding the two DDR reset writes
from [NXP's redundant-boot fix](https://community.nxp.com/t5/i-MX-Security/BootROM-HABv4-Redundant-boot-support-is-not-triggered-when-first/ta-p/1119740)
to the secondary DCD made that image boot successfully. The primary DCD
was not changed.

| Test | Result |
|---|---|
| Force secondary with `SRC_GPR10[30]`, then warm reset | U-Boot starts; ROM marker `0x51` |
| Invalidate primary IVT tag, clear forced selection, then reset | ROM selects secondary; U-Boot starts |

The automatic-fallback capture contains `0x51`, reads at sectors `0x800`
and `0x808`, and a jump to `0x87800000`. GPR10 bit 30 is set. Reading the
primary IVT back confirms that its tag remained invalid during this boot.
The primary tag was restored after the test. A final reboot reported
`0x50` again, with GPR10 bit 30 clear.

This validates fallback to a second image in the same eMMC boot partition.
It does not demonstrate a switch from `boot0` to `boot1`, or authentication
failure recovery on a closed device. The automatic-fallback log was captured
in U-Boot and added to the tool's regression tests.

## Linux validation after secondary boot

A further forced-secondary boot continued into Torizon OS with kernel
`6.6.143-7.7.0`. The static ARM build read the live ROM data through
`/dev/mem`, without a kernel or device-tree change.

| Command | Result | Exit |
|---|---|---|
| `romlog` | Secondary image, authentication result and jump address | 0 |
| `bootinfo` | Secondary image, MMC3, entry `0x87800000`, five HAB events | 0 |
| `hab_status` | Open device; invalid-CSF event and four assertion events | 1 |
| `reset-cause` | Unknown; `SRC_SRSR` cleared by the bootloader | 3 |

The HAB failure is expected for this unsigned image on an open device.
It must not be reported as successful authentication. JSON output and
repeated ROM-log reads were also checked.

A primary boot was checked in both U-Boot and Linux without a reset between
the captures. All five HAB event records matched byte for byte. The ROM
log retained the same primary marker, entry address and timestamps.

## Hardware-partition identity remains unknown

A primary/secondary image marker is not yet a proven hardware-partition
identifier. [U-Boot's i.MX7 redundant-boot documentation](https://docs.u-boot.org/en/latest/board/nxp/psb.html)
describes a Secondary Image Table and an image offset. We must distinguish
that path from switching between eMMC `boot0` and `boot1`.

The tool therefore reports the ROM image selection separately from
EXT_CSD configuration. It does not label either value as proof of the
hardware boot partition used.
