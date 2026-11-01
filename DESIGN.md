# imx-sectool design

| | |
|---|---|
| Author | Stefano Viola <stefano.viola@toradex.com> |
| License | MIT |
| First target | NXP i.MX8M Plus (Verdin iMX8M Plus) |

## 1. Purpose

On NXP i.MX processors, fuses and secure boot are handled from the U-Boot
console:

- `fuse read|sense|prog|override` reads and burns fuses;
- `hab_status` (HABv4: i.MX6/7/8M) and `ahab_status` (AHAB: i.MX8/8X,
  i.MX8ULP, i.MX9) show if the device is closed and if the Boot ROM logged
  authentication events.

These commands use hardware and firmware interfaces that Linux can also
reach: fuse registers, firmware mailboxes (SCU, EdgeLock Enclave), TF-A
services, and data that the Boot ROM leaves in on-chip RAM. Linux already
has fuse drivers (nvmem), but no tool brings all of this together.

imx-sectool is one C binary, with no dependencies, that gives Linux:

1. `fuse`, with the same command line as U-Boot, so existing scripts (for
   example the ones from meta-toradex-security) work without changes;
2. `hab_status` / `ahab_status`, with the same output and exit codes as
   U-Boot;
3. more information: how the board booted, the Boot ROM log, reset cause,
   firmware versions, chip identity and a security audit.

Why this is useful:

- Production and field service can provision and check devices over SSH,
  without a serial console.
- Closed devices often have a U-Boot without console. Today you need JTAG
  or a debug build to see if a device is really closed, or why a signed
  image gave HAB events. Linux usually still runs.
- Fleet tools (for example Torizon Cloud) can collect the security state of
  every device with `--json`.
- A/B update and recovery logic can see if the Boot ROM fell back to the
  secondary boot image. Linux does not report this today.

## 2. Goals and non-goals

Goals:

- C11, libc only, one binary, easy to cross-compile and package in Yocto.
- Same `fuse` and `hab_status` behaviour as U-Boot, including the Toradex
  patches (section 9). `ahab_status` also reports partial availability.
- One binary with several names: called as `fuse` or `hab_status`, it works
  like the U-Boot command.
- Works with existing OS interfaces where possible. AHAB events may use an
  optional mailbox overlay and stock UIO driver. Missing setup affects only
  that feature: the command says `unavailable` and why (section 5.4).
- Safe defaults for anything that cannot be undone (section 7).
- `--json` for every command that is not a U-Boot clone.
- All i.MX families over time, starting with the i.MX8M Plus.

Non-goals:

- Image signing and CSF generation (use NXP CST or crucible's `habtool`).
- Key management and HSM integration.
- Crypto services of the EdgeLock Enclave (NXP's `imx-secure-enclave` does
  that). Only boot, fuses and lifecycle are in scope.
- A custom kernel module as a requirement for the tool. The optional AHAB
  mailbox backend uses the existing UIO driver.
- i.MX RT microcontrollers.

## 3. Existing tools

| Project | What it does | What is missing |
|---|---|---|
| [usbarmory/crucible](https://github.com/usbarmory/crucible) (Go) | Read and burn fuses by name through nvmem, YAML fusemaps (also i.MX8MP); `habtool` signs HABv4 images | No HAB/AHAB status or events, no ELE/SCU, not the U-Boot syntax |
| [phytec/efusemem](https://github.com/phytec/efusemem) (C, MIT, beta) | Fixed operations through nvmem: SRK hash, MAC, revoke, lock secure boot / SDP / JTAG on 6ULL/6Q/8MM/8MP | No status or events, OCOTP SoCs only |
| [nxp-imx/imx-secure-enclave](https://github.com/nxp-imx/imx-secure-enclave) `se_tools` | ELE only: info, events, lifecycle update, commit | ELE SoCs only, needs the NXP library, not U-Boot compatible |
| `dd` on the nvmem file | Raw word access | No names, no safety checks, no status |

No Linux tool reports HAB events, the i.MX8M ROM log or how the board
booted, and none does a security audit. imx-sectool credits crucible and
efusemem. crucible's fusemaps are BSD-3-Clause, which works with MIT when
the notice is kept.

### 3.1 Features of the other tools

imx-sectool should not do less than these tools, but it does not copy
them. For each feature we decide to **adopt** it, **improve** it, **adapt**
it to a different form, or **skip** it. Only the U-Boot syntax is copied
exactly, because compatibility is the reason it exists.

Common rules: one subcommand style instead of many flags; values shown with
their meaning and the raw hex; destructive commands show the value before
and after; `--json` everywhere except the U-Boot clones; every error says
why and what to do.

| Feature (from) | Decision | Status |
|---|---|---|
| `fuse` sub-commands, Toradex `readv` and `-q` (U-Boot) | adopt: same syntax and output | done (`sense`/`override` report unavailable) |
| `hab_status`, `ahab_status` (U-Boot) | adopt, with the Toradex exit codes | `hab_status` done |
| `ahab_close`, `ahab_commit` (U-Boot) | improve: `provision close` and `ele commit`, with checks | planned (ELE) |
| Fuses by name (crucible) | improve: SoC detected automatically, names accepted directly by `fuse` | done |
| List fusemaps (crucible) | improve: `fusemap` shows each field with its meaning | done |
| Base / endianness flags (crucible) | adapt: the SoC table knows the format of each field | done |
| Custom fusemaps (crucible) | adapt: optional `--fusemap` overlay | planned |
| Syslog, value-only output (crucible) | improve: `prog` always logs to syslog; `-q` and `--json` for scripts | done |
| No-prompt `-Y` (crucible) / `-y` (U-Boot) | adopt U-Boot's `-y` | done |
| SRK hash read/write (efusemem) | improve: `srk show`, `srk prog`, `srk verify` against `SRK_fuse.bin` or an SRK table | planned |
| MAC read/write (efusemem) | improve: `mac show|prog`, refuses invalid or already set values | read done (`fusemap MAC0`) |
| SRK revoke (efusemem) | improve: refuses to revoke the last valid key | planned |
| Lock secure boot / SDP / JTAG (efusemem) | improve: `provision close | disable-sdp | secure-debug <mode>` with checks first | planned |
| Lock status (efusemem) | adapt: part of `audit` and `debug-status` | done |
| Device info: UID, lifecycle, SRK hash, FW hash (se_tools) | adapt: `identity`, `fwinfo`; UUID ready for `TDX_SECURE_DEBUG_ELE_DC_UUID` | planned (ELE) |
| Events (se_tools) | adapt: `ahab_status` and `ele events --json` | planned (ELE) |
| Lifecycle update, commit (se_tools) | improve: show what changes, check first | planned (ELE) |
| Attestation (se_tools) | adapt later: `ele attest` | later |
| RNG (se_tools) | skip: Linux has `/dev/hwrng` | — |
| DEK / OTFAD blobs, NVM daemon (se_tools) | skip: key management, out of scope | — |
| Signing, SRK tables, CSF (habtool) | skip: host side; `srk verify` covers the device side | — |

New in imx-sectool: HAB events from Linux, `bootinfo`, `romlog`,
`reset-cause`, `debug-status`, `audit --profile`, and (planned)
`fuse-script` to run the fuse files generated by meta-toradex-security.

## 4. How U-Boot does it

### 4.1 `fuse`: three different backends

| SoC family | U-Boot | How | Linux driver |
|---|---|---|---|
| i.MX6/7, 7ULP, 8M | `drivers/misc/mxc_ocotp.c` | OCOTP registers (unlock key, write, wait, reload shadows) | `imx-ocotp.c`: read and write |
| i.MX8QM / 8QXP / 8DXL | `drivers/misc/imx8/fuse.c` | SCU firmware calls | `imx-ocotp-scu.c`: read through the SCU, write through TF-A (SMC `0xC200000B`) |
| i.MX8ULP, i.MX9 | `drivers/misc/imx_ele/fuse.c` | ELE messages: `READ_FUSE 0x97`, `WRITE_FUSE 0xD6` | `imx-ocotp-ele.c`: read only. The `se_ctrl` device can send any ELE message |

U-Boot sub-commands:

| Command | Meaning | From Linux |
|---|---|---|
| `read` | Read the shadow register | nvmem read |
| `sense` | Read the fuse array directly | needs OCOTP registers (not done, see 5.2) |
| `prog` | Burn a fuse word | nvmem write |
| `override` | Write the shadow only, lost at reset | needs OCOTP registers (not done) |
| `cmp` | Compare a word with a value | nvmem read |
| `readm` | Read into memory | prints the words |
| `readv` (Toradex) | Read into an environment variable | prints `name='value'` for `eval` |

### 4.2 `hab_status` (HABv4)

U-Boot calls Boot ROM functions (`report_status`, `report_event`) through
the HAB ROM vector table. They read the ROM's event area in OCRAM.

- i.MX6/7: U-Boot calls the ROM directly.
- i.MX8M: U-Boot runs at EL2 and asks TF-A with SMC `0xC2000007`. Linux
  userspace cannot make this call.
- i.MX7ULP M4: U-Boot reads the HAB event area directly, without the ROM.
  imx-sectool does the same on i.MX8M (section 8).

"Secure boot enabled" is just the `SEC_CONFIG` fuse.

### 4.3 `ahab_status`

- ELE (i.MX8ULP, i.MX9): lifecycle from a register or a fuse, events with
  `ELE_GET_EVENTS 0xA2`. `ELE_GET_INFO` gives lifecycle, UID, SRK hash and
  firmware state.
- SCU (i.MX8QM/QXP): lifecycle and firmware version through the SECO HSM
  device. Events use SCU RPC through an optional dedicated UIO mailbox on
  i.MX8QM and i.MX8QXP. Missing events are unavailable, never treated as
  a clean result.
  See [AHAB setup](docs/ahab-uio.md) and the
  [interface audit](docs/ahab-linux-interfaces.md).

## 5. Architecture

### 5.1 Overview

```
     argv[0] = fuse | hab_status | ahab_status | imx-sectool <cmd>
                               |
                       +---------------+
                       |   commands    |  U-Boot compatible parsers,
                       |   (cmd_*.c)   |  text and --json output
                       +-------+-------+
                               |
                       +-------v-------+
                       |   SoC table   |  fuse layout, named fields,
                       |   (soc.c)     |  allowed memory regions
                       +-------+-------+
                               |
       +------------+----------+---------+------------+
       |            |                    |            |
  +----v----+  +----v-----+        +-----v----+  +----v-----+
  | A nvmem |  | B/C mmio |        |  D ELE   |  | E kernel |
  |         |  | /dev/mem |        | se_ctrl  |  | (future) |
  +---------+  +----------+        +----------+  +----------+
```

The SoC is read from `/sys/devices/soc0/soc_id` (`--soc` overrides it). It
selects a table compiled into the binary: fuse layout, named fields,
register addresses and the memory regions that may be read. No data files
are needed at run time.

### 5.2 Access classes

| Class | Source | Used for | Risk |
|---|---|---|---|
| A: nvmem | `/sys/bus/nvmem/devices/*/nvmem` | all fuse reads and writes | low: the driver handles locking and clocks |
| B: registers | `/dev/mem` | SRC boot mode and reset registers, SNVS state | medium: a block with its clock off can hang the bus |
| C: OCRAM data | `/dev/mem` on ROM and OCRAM | Boot ROM log, HAB events, i.MX9 ROM passover data | medium: the data can be old or overwritten |
| D: enclave driver | `se_ctrl` device | SECO lifecycle and version; ELE fuses, events and info (planned) | low to medium: the firmware checks requests |
| UIO: optional SCU mailbox | `/dev/uioN` with overlay | SECO lifecycle and AHAB events on i.MX8QM/QXP | dedicated mailbox and managed power required |
| E: kernel helper (future) | optional module or upstream driver | TF-A HAB call on i.MX8M, SCU security calls | not in the first version |

Rules for B and C:

- Only regions listed in the SoC table can be read. The user never gives an
  address.
- `/dev/mem` is opened read-only and mapped with `PROT_READ`. Reads are
  aligned 32-bit loads.
- OCOTP registers are never accessed directly: Linux turns off the OCOTP
  clock when idle, and access with the clock off can hang. This is why
  `fuse sense` and `override` are not available.
- OCRAM data is always checked before use: pointer range, tag, version,
  record length and alignment. If a check fails, the result is `untrusted`.

### 5.3 Sources per family

| Family | fuse | hab/ahab status | bootinfo, romlog | reset-cause |
|---|---|---|---|---|
| i.MX6/7 | A | fuse + SNVS; events via C (found on i.MX7D) | SRC (B); i.MX7D ROM log (C) | SRC (B), cleared by U-Boot |
| i.MX8M | A | fuse + SNVS; events via C | SRC (B), ROM log (C) | SRC (B), not reliable on 8MP |
| i.MX8QM/QXP | A (planned) | lifecycle through D; events through optional UIO | not implemented | — |
| i.MX8ULP, i.MX9 | D (A for read) | D | ROM passover data (C) | SRC (B) |

### 5.4 When data is not available

Each command finds its sources at run time. A result is:

- `ok`: data read and checked;
- `unavailable`: source not reachable (driver missing, `/dev/mem` blocked,
  SoC not supported), always with the reason and a hint;
- `untrusted`: data read but not valid.

Example:

```
$ imx-sectool romlog
error: ROM log unavailable: cannot open /dev/mem (Permission denied); run as root
```

Commands that use several sources, like `audit`, show what they could read
and mark the rest as `unavailable`, never as `pass`.

## 6. Commands

### 6.1 Command line

```
imx-sectool [--json] [--soc <name>] [--nvmem <path>] <command> [args]
fuse        <U-Boot fuse args>            # link to imx-sectool
hab_status                                # link to imx-sectool
```

### 6.2 `fuse`

```
fuse read     [-q] <bank> <word> [<cnt>]
fuse readv    [-q] <bank> <word> <name> [<cnt>]
fuse readm    [-q] <bank> <word> <addr> [<cnt>]   # <addr> ignored
fuse cmp      [-q] <bank> <word> <hexval>
fuse prog     [-y] <bank> <word> <hexval> [<hexval>...]
fuse sense         <bank> <word> [<cnt>]          # reports unavailable
fuse override      <bank> <word> <hexval> [...]   # reports unavailable
```

- Numbers are parsed like U-Boot: `strtoul(..., 0)` for bank, word and
  count; hex for values.
- Messages are the same as U-Boot, byte for byte, so log parsers keep
  working.
- A field name can replace `<bank> <word>`: `fuse read JTAG_SMODE`,
  `fuse prog -y KTE 1`. U-Boot would reject these lines, so this does not
  change the U-Boot syntax. Values are field values. Names ignore case and
  brackets (`SEC_CONFIG1` for `SEC_CONFIG[1]`).
- Exit codes: `0` success or `cmp` equal, `1` failure or `cmp` different,
  `2` usage error.

Planned: `imx-sectool fuse-script [--dry-run] [-y] <file>` runs the
`fuse prog` files from meta-toradex-security, stops at the first error,
and in dry-run mode shows the current, requested and resulting values.

### 6.3 `hab_status` / `ahab_status`

- Same output as U-Boot: `Secure boot enabled|disabled`,
  `HAB Configuration: 0x.., HAB State: 0x..`, then each event or
  `No HAB Events Found!`.
- On i.MX8M the HAB state comes from the SNVS state and the events from the
  Boot ROM area in OCRAM.
- If the events cannot be read, the command says so and exits with
  `EX_UNAVAIL`. It never prints `No HAB Events Found!` unless it read them.
- Exit code as in the Toradex U-Boot: `1` if there are events.

### 6.4 Other commands

| Command | Shows | Status |
|---|---|---|
| `ahab_status` | SECO lifecycle, firmware version and AHAB events | lifecycle and optional UIO events on i.MX8QM/QXP |
| `bootinfo` | Boot mode registers, boot config source, primary or secondary image, authentication result, secondary image offset, secure boot. The boot device is not shown: on i.MX8M Plus only the bootloader can ask the ROM | done |
| `romlog` | i.MX7Dual and i.MX8M Plus Boot ROM event log with timestamps | done |
| `reset-cause` | `SRC_SRSR` with U-Boot's names | done (not reliable on 8MP) |
| `debug-status` | JTAG / secure debug mode (section 6.5) | done |
| `fusemap` | Named fields with their meaning, one register, SRK hash, MAC addresses | done |
| `audit` | Secure boot and secure debug checklist | done |
| `identity` | SoC, revision, UID, speed and temperature grade | planned |
| `fwinfo` | ELE / SCU firmware versions, OP-TEE version | planned |
| `srk`, `mac` | Read, program and verify SRK hash and MAC | planned |
| `provision` | `close`, `disable-sdp`, `secure-debug <mode>` with checks | planned |
| `ele` | `info`, `events`, `commit`, `lifecycle`, later `attest` | planned |
| `caps` | What this system can read | planned |

`audit` checks:

1. Secure boot closed (`SEC_CONFIG` or lifecycle).
2. No HAB/AHAB events.
3. SRK hash programmed and locked.
4. SRK revocation state.
5. Serial downloader disabled, boot from fuses.
6. Debug policy from `debug-status`.
7. Boot config and other locks.
8. SNVS state.
9. Field return.

### 6.5 `debug-status`

This checks on a running device that the secure debug feature of
meta-toradex-security (`INHERIT += "tdx-secure-debug"`) was provisioned as
configured. It reports the mode with the class's names
(`TDX_SECURE_DEBUG_MODE` = `authenticated` / `no-debug`,
`TDX_SECURE_DEBUG_SJC_DISABLE`, `TDX_SECURE_DEBUG_ELE_JTAG_DISABLE`) next to
the fuse values. Everything comes from fuses, so no `/dev/mem` is needed.

The rules follow `create_fuse_cmds.sh` in the layer.

i.MX6/7/8M (SJC, `JTAG_SMODE` in bank 1 word 3 bits 23:22):

| Fuses | Mode |
|---|---|
| `SJC_DISABLE` set | `SJC_DISABLE=1` (JTAG fully off) |
| `JTAG_SMODE = 11` | `no-debug` |
| `JTAG_SMODE = 01`, response key programmed and locked | `authenticated` (`SJC_HEO=1` if `JTAG_HEO` is set) |
| `JTAG_SMODE = 00`, no `SJC_DISABLE` | not provisioned (JTAG open) |

Warnings when something is missing: `BOOT_CFG_LOCK` in every mode, `KTE` in
`authenticated` and `no-debug`, `SJC_RESP_LOCK` when a key is present. On
i.MX8M Plus `JTAG_SMODE = 01` is an error: NXP removed secure JTAG there
(ERR052318) and the layer does not allow it.

i.MX8/8X (SJC, active only when the lifecycle is OEM Closed):

| Fuses | Mode |
|---|---|
| `SJC_DISABLE` (bank 0 word 5 bit 0) | `SJC_DISABLE=1` |
| `CHALLENGE_RESP_DISABLE` and `TZ_CHALLENGE_RESP_DISABLE` | `no-debug` |
| OEM key and lock; TZ key and lock, or TZ challenge disabled | `authenticated` |
| none | not provisioned |

i.MX9 (ELE):

| Fuses / lifecycle | Mode |
|---|---|
| all `DBG_DISABLE_*` and `JTAG_DISABLE` | `ELE_JTAG_DISABLE=1` |
| all `DBG_DISABLE_*` | `no-debug` |
| some `DBG_DISABLE_*` | partial (warning) |
| none, OEM Closed | `authenticated` (the layer burns no fuses for it, so this looks the same as "closed, secure debug not set") |
| none, OEM Open | open |

Checking against the build: the layer writes `imx-config.fuse` to the
deploy directory, with the values it asked to burn
(`SJC:JTAG_SMODE_NODEBUG:1:3:0x00C00000`, `H:F:...`, `H:C:...`).

```
$ imx-sectool debug-status --profile imx-config.fuse
$ imx-sectool audit        --profile imx-config.fuse
$ imx-sectool debug-status --expect no-debug && echo OK
```

For each row the tool checks `(fuse & value) == value`. Key rows
(`SJC_RESP*`, `OEM_KEY*`, `TZ_KEY*`) are never compared by value. A locked
key reads `0xBADABADA` and is reported as locked; a readable key is only
checked to be non-zero, with a warning about the missing lock.
`imx-config.fuse` has the response key in clear text, so we could propose
that the layer also writes a copy without keys, for use on devices.

Secrets:

- `debug-status` never prints the response key, only whether it is
  programmed and locked.
- A read-locked word reads `0xBADABADA`. It is always treated as "locked",
  never as data. `fuse read` prints it, like U-Boot.
- A programmed key that is not read-locked is an audit failure: any root
  process can read it.

Limits:

- This is the configured policy, not whether a debugger is connected now.
  The run-time state is in Arm registers that need EL1 or CoreSight access
  (open question 8).
- The shadow registers that Linux reads match the burned fuses unless
  someone used `fuse override` since reset.

### 6.6 Exit codes

For the commands that are not U-Boot clones:

| Code | Name | Meaning |
|---|---|---|
| 0 | `EX_OK` | Success, or every audit check passed |
| 1 | `EX_FAIL` | Failure, or an audit check failed |
| 2 | `EX_USAGE` | Wrong command line |
| 3 | `EX_UNAVAIL` | A source is not available |
| 4 | `EX_UNTRUSTED` | Data not valid |
| 5 | `EX_WARN` | Passed with warnings |

## 7. Fuse programming safety

1. `fuse prog` asks for confirmation unless `-y` is given, like U-Boot.
2. Before writing, it shows the current value, the requested value and the
   result. Burning a fuse can only set bits, so the result is
   `current | requested`.
3. If all requested bits are already set, nothing is written.
4. A read-locked word is not programmed.
5. After writing, the word is read back and compared.
6. Every write is logged to syslog (`LOG_AUTH`).

Planned:

- Refuse to write a word whose lock fuse is set, unless `--force`.
- On SCU parts, refuse a second write to ECC words (they can be written
  only once).
- `provision` checks first and prints what it checked:
  - `close`: SRK hash programmed and locked, no HAB/AHAB events;
  - `disable-sdp`: boot from fuses is set, so the board can still boot;
  - `secure-debug <mode>`: same fuses and order as meta-toradex-security
    (key, key lock, mode, HEO, KTE, `BOOT_CFG_LOCK` last); refuses modes
    the SoC does not support.

## 8. i.MX8M Plus

| Item | Value | Source |
|---|---|---|
| Fuse layout | 96 banks of 4 words; nvmem offset = `(bank * 4 + word) * 4` | U-Boot `mxc_ocotp.c`, Linux `imx-ocotp.c` |
| `SEC_CONFIG[1]` (closed) | bank 1 word 3, `0x02000000` | meta-toradex-security `imx8m-template.fuse` |
| SRK hash | banks 6 and 7 | same |
| Secure debug | `BOOT_CFG_LOCK` 0/0; `JTAG_SMODE`, `SJC_DISABLE`, `KTE` in 1/3. No secure JTAG (ERR052318) | `imx8mp-sjc-template.fuse` |
| Read-locked word | `0xBADABADA` | Linux `imx-ocotp.c` |
| SRC | `0x30390000`: `SBMR1` +0x58, `SRSR` +0x5c, `SBMR2` +0x70, `GPR10` +0x98 | U-Boot |
| SNVS | `0x30370000`: `HPSR` +0x14, SSM state in bits 11:8 | U-Boot |
| HAB ROM vector table | `0x900` | TF-A |
| ROM log pointer | `0x9e0`, points into OCRAM | U-Boot, NXP AN12853 |
| HAB event area | `0x0090d040` (`EVT_DEF`, tag `0x0C`), events (tag `0xDB`) after it | found on hardware |
| OCRAM read window | `0x900000` to `0x940000`, below TF-A (NXP BL31 at `0x950000`) | TF-A `platform_def.h` |

All values were checked on hardware against U-Boot on the same boot (see
[docs/phase0-imx8mp.md](docs/phase0-imx8mp.md)).

`SRC_SRSR` reads `0x1` after every kind of reset on this board, so
`reset-cause` prints a warning.

### i.MX8M Mini

Differences from the i.MX8M Plus:

| Item | Value | Source |
|---|---|---|
| Fuse layout | 64 banks of 4 words | Linux `imx-ocotp.c` |
| SRK hash lock | `SRK_LOCK`, bank 0 word 0 bit 9 | crucible IMX8MM fusemap |
| `SRK_REVOKE` | 3 bits, bank 9 word 3 | same |
| Secure debug | Secure JTAG (`JTAG_SMODE=01`) is supported | meta-toradex-security |
| `TEMP_GRADE` | bank 1 word 0 bits 7:6 | U-Boot `get_cpu_temp_grade()` |
| `SPEED_GRADING` | bank 1 word 0 bits 10:8: 800, 1200, 1600, 1800, 2000 MHz | U-Boot |
| TF-A | BL31 at `0x920000` | TF-A `platform_def.h` |

The fuse commands were checked on an open Verdin iMX8M Mini. No `/dev/mem`
region is enabled yet, so `hab_status`, `romlog`, `bootinfo` and two audit
checks report "unavailable".

### i.MX7Dual

A read-only investigation found the ROM boot log now used by `romlog`
and `bootinfo`. See [the findings and fallback tests](docs/imx7-boot-log.md)
and [the secondary-boot and rollback design note](docs/imx7-secondary-boot.md).

Differences from the i.MX8M Plus:

| Item | Value | Source |
|---|---|---|
| Fuse layout | 16 banks of 4 words | Linux `imx-ocotp.c` |
| SRK hash lock | `SRK_LOCK`, bank 0 word 0 bit 9 | crucible IMX7D fusemap |
| `SRK_REVOKE` | 3 bits, bank 9 word 3 | same |
| Secure debug | Secure JTAG (`JTAG_SMODE=01`) is supported | meta-toradex-security `imx7-sjc-template.fuse` |
| `SDP_DISABLE` | bank 2 word 0 bit 9 | crucible IMX7D fusemap |
| `SPEED_GRADING` | bank 1 word 0 bits 9:8: 800, 500, 1000, 1200 MHz | U-Boot |
| HAB ROM vector table | `0x100` | U-Boot `hab.h` |
| HAB event area | store 0, `0x009049c0` to `0x0090553f`; event records 4-byte aligned | ROM store table at `0x11598`, found on hardware |
| OCRAM read windows | `0x904000` to `0x906000`; boot pointer limited to `0x900000` to `0x920000` | HAB store and ROM software info |
| Boot device | software-info pointer at ROM `0x1e8` | U-Boot `ROM_SW_INFO_ADDR` and `get_boot_device()` |
| Boot selection | `SRC_GPR10[30]`: primary or secondary | U-Boot `boot_mode_getprisec()` |
| ROM log | pointer at `0x1e0`; 64-word buffer | [read-only investigation](docs/imx7-boot-log.md) |

The HAB store has the same records as on the i.MX8M Plus: `EVT_DEF` first,
then the events. The ROM's `report_event()` reads its records from store 0.
All five events on an open board matched U-Boot byte for byte on the same boot.

Linux shows `ocram_clk` with an enable count of 0, but the clock gate
(`CCGR17`, CCM + `0x4110`) reads `0x2`, so OCRAM is clocked and can be read.

Broad Boot ROM reads from Linux hung this board and triggered watchdog
resets. A single aligned 32-bit read at `0x1e8` succeeded, followed by
20 matching reads without a reset. The log pointer at `0x1e0` was also read successfully. The tool allows
these two words, not ROM scans. Each read accesses only one 32-bit word.

U-Boot clears `SRC_SRSR` after it reads it, so `reset-cause` usually reports
"unknown". A nonzero value is decoded with a warning; it may not describe
the reset that started this boot.

`bootinfo` follows the ROM software-info pointer, as U-Boot does. It checks
alignment and that the 32-byte structure fits inside main OCRAM before
reading its boot-device word. Unknown device codes and invalid SD/MMC
instances report unavailable. No fixed software-info address is assumed.

On the tested i.MX7Dual board (NXP silicon revision 1.3), the ROM pointer
was `0x009090c4` and the device was MMC3. The tool follows the pointer
without checking the chip revision.
SD/MMC instances use controller numbering, not Linux `/dev/mmcblkN`
numbering. The saved data can still be overwritten after boot.

`bootinfo` also shows the primary/secondary selection from `SRC_GPR10`
and the HAB event count. The selection can be changed by U-Boot and does
not prove which image ran. No events does not prove authentication succeeded.
The ROM log supplies the image selection, authentication result and entry
address. A secondary marker does not prove automatic fallback or identify
an eMMC boot partition. A failure to read the log, device, selection or
HAB events returns exit 3.

The HAB parser rejects malformed, truncated and oversized event records,
and lists beyond its capacity. These produce unavailable results rather
than a clean `hab_status` or audit PASS. A valid header alone cannot prove
that OCRAM has not been overwritten; the empty-store and closed-device
paths still need hardware validation.

Checked on an open Colibri iMX7D: the fuse values, the SNVS state and the
five HAB events (`HAB_INV_CSF`, then four `HAB_INV_ASSERTION`) that an open
board shows with an unsigned image.

## 9. meta-toradex-security

| In the layer | In imx-sectool |
|---|---|
| `imx-fuses` writes `fuse prog -y <bank> <word> <val>` files | planned `fuse-script` runs them from Linux, with a dry run first |
| Fuse templates and `imx-config.fuse` | `audit --profile` and `debug-status --profile` check every fuse |
| U-Boot patches for `fuse readv` and `fuse -q` | same behaviour |
| U-Boot patches for `hab_status` / `ahab_status` exit codes | same exit codes |
| `tdx-secure-debug` class | `debug-status` reports the mode with the class's names |
| Secure boot and secure debug docs | could add a "Check from Linux" step with `imx-sectool audit` |

A recipe `imx-sectool_git.bb` could live in the layer.

## 10. Testing

- Host tests: `make check` builds synthetic fuse and memory dumps
  (`test/mkfixtures.py`) and compares the output of each command with
  `test/expected/`. The fixtures use a fake UID.
- Every backend can read from a file instead of the hardware
  (`--nvmem <file>`, `IMX_SECTOOL_MEM_DUMP=<dir>`, `IMX_SECTOOL_SOC`).
- Hardware: read-only commands are checked against U-Boot on the same boot.
  Fuse writes are tested on a fake nvmem file. A real fuse is burned only
  on a board set aside for it, on a general-purpose word.
- A new SoC starts with the fuse commands only. Its `/dev/mem` regions are
  enabled after they are checked on hardware.

## 11. Future work

- i.MX8M Mini: check the Boot ROM regions and the HAB event area on
  hardware.
- i.MX8M Nano and Quad, then i.MX9 (ELE), then i.MX6 and i.MX7Solo, then
  SCU fuse support and event access on other SCU parts (i.MX8/8X).
- i.MX7D: test a closed board.
- Optional kernel helper for the TF-A HAB call on i.MX8M and the SCU
  security calls, proposed upstream as a sysfs interface.
- U-Boot could copy the HAB events and ROM log into the device tree
  (`/chosen`); imx-sectool would use them when present.
- Decode the ROM log parameters and timestamps (AN12853) and the boot
  device.
- Check if `WDOG1_WRSR` keeps the reset cause that `SRSR` loses.
- Torizon Cloud metadata from `audit --json`.

## 12. Open questions

1. `/dev/mem` policy: some kernels block ROM and OCRAM. Then those
   features are `unavailable` by design.
2. Data survival: TF-A and some Linux drivers use OCRAM. Check per SoC and
   boot flow.
3. HAB event area on i.MX6/7: address not documented, must be found on
   hardware.
4. i.MX6 nvmem offsets: Linux reads include the shadow register holes but
   writes use the fuse index. The SoC table must handle both.
5. ELE from Linux: check that ELE accepts `WRITE_FUSE` and lifecycle
   commands from the Linux mailbox.
6. crucible fusemaps: convert the YAML to C tables on the host and commit
   them, keeping the notice.
7. The tool makes destructive operations easier from a root shell. Closed
   products should disable nvmem writes and restrict `/dev/mem` and the ELE
   device.
8. Run-time debug state: can `EDAUTHSTATUS` be read safely through
   `/dev/mem` on i.MX8M Plus, or does it need a kernel helper?

## 13. References

- U-Boot (NXP): `cmd/fuse.c`, `drivers/misc/mxc_ocotp.c`,
  `drivers/misc/imx8/fuse.c`, `drivers/misc/imx_ele/`,
  `arch/arm/mach-imx/{hab.c,ele_ahab.c,imx8/ahab.c,imx8m/soc.c}`.
- Linux (NXP lf-6.6.y): `drivers/nvmem/imx-ocotp*.c`,
  `drivers/firmware/imx/se_ctrl.c`.
- TF-A (NXP lf_v2.10): `plat/imx/common/imx_sip_svc.c`,
  `plat/imx/imx8m/imx_hab.c`.
- NXP AN12853 (i.MX ROM log events), AN4581 (HABv4 secure boot).
- meta-toradex-security: `recipes-bsp/imx-fuses`, `recipes-bsp/u-boot`,
  `docs/`.
- usbarmory/crucible, phytec/efusemem, nxp-imx/imx-secure-enclave.
