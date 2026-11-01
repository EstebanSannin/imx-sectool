# AHAB events from Linux on i.MX8QM and i.MX8QXP

## Result

No supported userspace interface for reading the AHAB event list was found
in the configuration checked. Lifecycle and firmware information are
available through the existing SECO driver. These do not replace the event
list: lifecycle alone cannot tell us whether image authentication failed.

This is a finding about the sources and system below, not a claim that every
vendor kernel or future firmware lacks an interface.

## Scope

The goal is to run a userspace tool without changing the device tree,
installing a module, or replacing Linux or U-Boot.

The review covered the Toradex Linux 6.6.142 base at
`84994b1002cbc19b3bb16c6ff18c4f1a48365957`, including all driver references
to the SCU RPC API, and NXP's imx-secure-enclave library at
`d7e1180b7f7d9dee1fa5e3982e7fad3f51f49939`.
The live interface inventory came from an i.MX8QM running
`6.6.142-rt75-7.7.0-devel`. The source review is of the base tree, not a
verified source match for every change in that RT build.

## Interfaces checked

| Interface | What it provides | AHAB events |
|---|---|---|
| SCU core | Kernel RPC transport and locking | No generic userspace RPC interface |
| SCU nvmem | Fuse reads and supported fuse writes | No event query |
| SECO HSM/SHE devices | SAB messages, including session-based lifecycle information | No supported event request found |
| SECO signed-message ioctl | A signed message passed through SCU `SAB_MSG` | Not an arbitrary SCU RPC interface |
| Security-violation device and debugfs | SNVS security and tamper state | Different information from AHAB authentication events |
| SoC and firmware sysfs | Identification and wakeup sources | No event export |
| Mailbox test driver | Debugfs messaging for a configured test mailbox | Disabled on the tested kernel; requires a mailbox client in DT |
| OP-TEE | Services supplied by installed secure firmware | No TEE device present on the tested system |
| RPMsg | Services supplied by remote firmware | No active endpoint providing this service |
| Boot properties and pstore | Information previously exported or logged | No AHAB event data present |
| UIO on a separate SCU mailbox | Userspace SCU requests | Worked after a DT overlay; does not meet the unchanged-system goal |

The other SCU clients cover clocks, power domains, pin control, thermal,
RTC, input and media. Their public interfaces select fixed services;
they do not forward arbitrary SCU requests.

## Details that matter

U-Boot reads lifecycle with SCU SECO function 17 (`CHIP_INFO`) and events
with function 19 (`GET_EVENT`). Neither helper is implemented in the
audited Linux SECO service API. The existing kernel RPC transport can
carry them, but it has no userspace entry point for these requests.

The HSM/SHE character devices use a different SECO protocol. NXP's SECO
message table marks `GET_EVENT` as unsupported. The ELE event command is
not interchangeable with the SCU request. An earlier direct test returned
an invalid-message response on SECO.

The signed-message ioctl is enabled only for i.MX8DXL HSM/SHE interfaces
in the audited initialization code. Its handler uses SCU function 23
(`SAB_MSG`), not a caller-selected SCU function. No malformed signed
messages were sent as part of this review.

The library's debug-dump message table looks promising, but its actual
API implementation explicitly returns an error on SECO. A message name
in a shared table is not enough to establish firmware support.

Security-violation `GET_STATE` reads HPSVS, LPS and LPTDS. An empty result
there must not be reported as an empty AHAB event list. The state-clear
and notification-triggering ioctls are not needed for this investigation.
The device was present in the live inventory. A follow-up read-only ioctl
check did not complete over the remote connection, so no new register
values are claimed here.

OP-TEE was also checked as a possible alternate service. Its presence in
the kernel configuration does not establish that secure firmware or a
suitable trusted application is available. No usable endpoint was found.

## What the MU2 test established

With a DT overlay and the stock UIO driver, a separate SCU mailbox returned
both lifecycle and an authentication event. This proves that the tested
firmware implements the requests. The overlay supplies the device and
power-domain binding; UIO keeps the domain powered while the device is open.

Mapping registers through `/dev/mem` alone does not supply that power or
ownership. Using the kernel's active SCU mailbox directly would also bypass
its request lock. Neither is an equivalent replacement for a dedicated,
managed mailbox.

## Implications for imx-sectool

Lifecycle support can use the existing SECO driver where it is available.
Event support must report unavailable until a working event backend exists.
It must not report a clean authentication result from lifecycle alone.

The demonstrated alternatives are a dedicated mailbox exposed through UIO,
a kernel interface, or a bootloader export. Each needs system integration.
No change to those components is part of this source audit.

## Source locations

- Linux: `drivers/firmware/imx/{imx-scu.c,seco.c,seco_init.c,se_ctrl.c}`.
- Linux: `drivers/soc/imx/secvio/`, `drivers/nvmem/imx-ocotp-scu.c`,
  `drivers/mailbox/mailbox-test.c`, and `include/uapi/linux/se_ioctl.h`.
- NXP library: `src/plat/seco/sab_msg.def` and
  `src/common/hsm_api/hsm_debug_dump.c`.
- [NXP secure-enclave library](https://github.com/nxp-imx/imx-secure-enclave).
- [NXP OP-TEE sources](https://github.com/nxp-imx/imx-optee-os).
