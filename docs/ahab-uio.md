# AHAB status through an optional SCU mailbox

`ahab_status` reads lifecycle and SECO firmware information through the
existing `/dev/hsm0_ch0` driver. On i.MX8QM, it can also read AHAB events
through a dedicated mailbox exposed by the stock UIO driver.

The mailbox setup is optional. Without it, lifecycle information remains
available and the command reports events as unavailable. It never installs
an overlay, loads a module or changes driver bindings itself.

```sh
sudo imx-sectool ahab_status
sudo imx-sectool ahab_status --json
sudo imx-sectool ahab_status --uio /dev/uioN
```

The tool finds UIO devices by the name `scu-mu2`, not by their number.
Use `--uio` only to select a particular device.

## Requirements

- i.MX8QM, with SCFW assigning MU2 to the Linux partition.
- MU2 dedicated to this tool. It must not be used by a DSP, remote core,
  kernel mailbox client or another userspace application.
- `CONFIG_UIO`, `CONFIG_UIO_PDRV_GENIRQ` and the i.MX SCU power-domain driver.
- The supplied overlay, applied using the board's normal boot configuration.
- Mounted debugfs with readable `pm_genpd/mu_a2/current_state`. The current
  backend checks this before touching registers.
- Root access. The command uses a lock and request marker under `/run`.

The stock UIO driver may be built in or supplied as a module by the OS.
No custom module is needed. If the OS does not include this driver, the
backend is unavailable; the overlay alone is not enough.

UIO event access is enabled only for i.MX8QM. i.MX8QXP currently has
lifecycle and firmware information only. Other commands on these two SoCs
remain unavailable; this change does not add fuse programming support.

## Install the overlay

Build [imx8qm-mu2-uio.dtso](../overlays/imx8qm-mu2-uio.dtso):

```sh
dtc -@ -I dts -O dtb -o imx8qm-mu2-uio.dtbo overlays/imx8qm-mu2-uio.dtso
```

The base DT needs the `lsio_mu2` and `pd` symbols. The overlay enables MU2
at `0x5d1d0000`, names it `scu-mu2` and attaches power domain
`IMX_SC_R_MU_2A` (215). It leaves the existing SCU mailbox alone.

Install the DTBO through the board's supported overlay mechanism and
reboot. For images using `overlays.txt`, append its filename to the
existing overlay list. Keep a backup of that list; do not replace the
other overlays. On systems with OS deployments, update the boot files for
the intended deployment.

Load the stock driver if it is a module:

```sh
sudo modprobe uio_pdrv_genirq of_id=generic-uio
```

To keep that module configuration across reboots, use the OS's module-load
configuration and set `options uio_pdrv_genirq of_id=generic-uio`. For a
built-in driver, the corresponding kernel argument is
`uio_pdrv_genirq.of_id=generic-uio`.

Check the binding:

```sh
for d in /sys/class/uio/uio*; do
    [ -r "$d/name" ] || continue
    printf '%s: ' "$d"
    cat "$d/name"
done
```

On the tested image, explicit binding was needed even with `of_id` set.
If MU2 is unbound, its DT node has the supplied `generic-uio` compatible,
and its power-domain reference is present, run:

```sh
sudo sh -eu <<'SH'
p=/sys/bus/platform/devices/5d1d0000.mailbox
[ ! -L "$p/driver" ]
[ "$(tr '\000' ' ' < "$p/of_node/compatible")" = 'generic-uio ' ]
[ "$(tr '\000' ' ' < "$p/of_node/linux,uio-name")" = 'scu-mu2 ' ]
printf '%s\n' uio_pdrv_genirq > "$p/driver_override"
printf '%s\n' 5d1d0000.mailbox > /sys/bus/platform/drivers/uio_pdrv_genirq/bind
printf '\n' > "$p/driver_override"
SH
```

An explicit binding does not survive reboot. If this workaround is needed,
package it as a boot-time setup step after loading the driver. Do not
unbind a driver already using MU2. Check ownership before installing the
overlay; the command cannot detect every remote firmware user.

## Results and failures

| Exit | Meaning |
|---|---|
| 0 | Lifecycle read and complete event list empty |
| 1 | Complete event list contains one or more events |
| 2 | Invalid command arguments |
| 3 | Required information unavailable, incomplete or inconsistent |

Exit 0 does not mean OEM secure boot is enabled. Lifecycle `NXP closed`
(`0x20`) is different from `OEM closed` (`0x80`).

JSON always includes `events_complete`. An empty `events` array with
`events_complete: false` is not a clean result. Events received before an
error are retained and marked incomplete.

The reader checks the hardware identity, UIO name, mapping, driver and
power state. It refuses pending traffic or enabled MU interrupts. It sends
only `CHIP_INFO` and `GET_EVENT`, without changing lifecycle or fuses.
The UIO device stays open to hold the power domain during access.

Each SCU request has a two-second timeout. A timeout, interrupted transfer
or malformed reply leaves `/run/imx-sectool-scu-mu2.lock` marked incomplete.
Reboot before retrying; do not delete the marker to bypass this check.
A software timeout cannot recover a hardware bus fault.

SCFW returns `SC_ERR_PARM` (3) after the last event on the tested firmware.
Only that response, with the expected reply layout, ends enumeration.
Other errors are reported. The reader stops before the 8-bit index wraps.
The end-of-list convention still needs validation on other SCFW versions.

## Validation and removal

Validated on i.MX8QM with Linux `6.6.142-rt75-7.7.0-devel` and SECO version
`0x00030085`: lifecycle, firmware version and event enumeration. Twenty
repeated queries passed alongside thermal reads. Missing UIO and lock
contention retained lifecycle information and returned exit 3. MU2 powered
down after closing. Host tests cover partial lists, firmware errors,
malformed responses, timeouts and index limits. An empty event log and an
OEM-closed device have not been tested on hardware.

To remove the setup, remove the overlay from the boot configuration and
any module-load or binding configuration added for it, then reboot.
The tool does not change that configuration.

See [the interface audit](ahab-linux-interfaces.md) for the alternatives
checked before choosing UIO.
