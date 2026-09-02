# honor-fan

Fan monitoring and control for the **HONOR MagicBook Pro 16 (2024, `DRA-XX`)** on Linux.

This laptop ships **no working fan interface** under Linux. There is no fan `hwmon`
device, and the five ACPI fans the firmware advertises are non-functional stubs. The
tachometers and the manual-speed override do exist — but only inside the embedded
controller, reachable through an undocumented memory-mapped command mailbox.

This project reverse-engineers that mailbox out of the machine's own ACPI tables and
implements it as a small `hwmon` kernel driver.

> **Read this first:** fan *monitoring* is solid. Fan *control* works but is not yet
> reliable — see [Status](#status). Control is therefore **opt-in**.

[中文文档](README.zh-CN.md)

---

## Hardware

| | |
|---|---|
| Model | HONOR MagicBook Pro 16 2024, `DRA-XX` (`M1040`) |
| CPU | Intel Core Ultra 7 155H (Meteor Lake) |
| GPU | NVIDIA RTX 4060 Laptop |
| EC | `\_SB.PC00.LPCB.H_EC`, Nuvoton |
| Fans | **2** physical fans |
| Tested on | Ubuntu 24.04, kernel 7.0 |

Likely applicable to other HONOR MagicBook / Huawei MateBook models that share this EC
firmware, but the addresses below are only verified on `DRA-XX`. Load with `force=1` to
try elsewhere — at your own risk.

## Why nothing standard works

| Path | Result |
|---|---|
| `hwmon` `fan*_input` / `pwm*` | **Absent.** No fan hwmon device exists at all. |
| ACPI fans `\_TZ_.FAN0..FAN4` (`PNP0C0B`, `cooling_device2-6`) | **Decoration.** `_ON`/`_OFF` set flags `CVF0..CVF4` and call `FNCL()`, which ends in `\_SB.PC00.LPCB.UPFS(...)` — an **empty method**. Writing `cur_state` does nothing. |
| `thermal` / DPTF | Reads temperatures only; no fan actuation. |
| `huawei_wmi` | Loads, but binds none of the fan GUIDs. `ABBC0F5B/5C-...` sit unclaimed. |
| `ec_sys` 256-byte EC space | Contains the temperature probes, but **no tachometer**. |

The fans are driven autonomously by the EC. The only handle is the EC itself.

## How it works

The firmware maps two EC windows into physical memory:

```
OperationRegion (ERAM, SystemMemory, 0xFE0B0300, 0x0100)   ; EC RAM mirror
OperationRegion (SMA2, SystemMemory, 0xFE0B0400, 0x0080)   ; command mailbox
```

`ERAM` mirrors the classic 256-byte EC space — verified, since offsets `0x20..0x2B` are
the temperature probes `TP00..TP0A` and match the kernel's `SEN1..SEN8`/`TCPU` zones
exactly. It holds no fan data.

`SMA2` is a mailbox giving access to *extended* EC RAM, where the fan registers live:

| Extended address | Meaning |
|---|---|
| `0x20020A00/01` | fan 1 tachometer, 16-bit LE |
| `0x20020A02/03` | fan 2 tachometer, 16-bit LE |
| `0x20020A04/05` | fan target speed, in units of 100 rpm |
| `0x20020A1B` | `FSUD` — manual-mode selector (0 = firmware, 1 = manual) |
| `0x20020A1C` | `FSVL` — manual speed value |

Reads use command `0x80`; writes need an unlock (`DATR=0x4D`, `CMDB=0x29`) followed by
command `0x81`. Full protocol in **[docs/PROTOCOL.md](docs/PROTOCOL.md)**, with the
supporting firmware source in **[docs/ASL-EVIDENCE.md](docs/ASL-EVIDENCE.md)**.

### The speed encoding

Not derivable from firmware — the ACPI method `SFCS` is never called with constants, so
it was measured. `FSVL` is an **increment above the fan's idle base, in units of 100 rpm**:

```
target_rpm = (base + FSVL) * 100        base = 16 (fan1), 14 (fan2)
```

Measured, exactly linear:

| FSVL | 10 | 20 | 30 | 40 | 50 | 60 | 80 | 100 |
|---|---|---|---|---|---|---|---|---|
| target rpm | 2600 | 3600 | 4600 | 5600 | 6600 | 7600 | 9600 | 11600 |

Physical ceiling is roughly 5800 rpm, so useful `FSVL` runs 0–42. `FSVL = 0` means idle
(~1600 rpm), **not** off.

### Two things that will bite you

1. **Both registers must always be written.** Setting `FSUD=0` alone to hand control back
   leaves a stale `FSVL` behind, and the EC latches with **both fans stopped**, ignoring
   further writes. `FSVL` must be zeroed too.
2. **The EC desyncs under back-to-back writes.** Each write is a full unlock+write mailbox
   handshake; issued microseconds apart, the EC stops responding and can leave the fans
   stopped. It recovers on its own after roughly **40 seconds of no mailbox traffic**.

## Status

| Feature | State |
|---|---|
| Read both fan tachometers | **Works reliably** |
| Read 11 EC temperature probes | **Works reliably** — cross-checked against kernel thermal zones |
| Set fan speed | **Works, not yet reliable.** Mapping solved and verified (e.g. 100% → 4920/4500 rpm), but the EC intermittently desyncs and stops the fans until left idle. |
| Return control to firmware | Same caveat as above |

Because of this, **`pwm1` is read-only by default**. Pass `enable_control=1` to unlock it.

Help wanted: the desync is the main open problem. Suspects are missing inter-transaction
delay, a required EC query-event notification that `SFCS` triggers indirectly, or a
handshake step not visible in the ASL.

## Install

Secure Boot puts the kernel in `lockdown=integrity`, which blocks every userspace route to
this hardware (`/dev/mem`, `ioperm()`, `ec_sys write_support=1`, unsigned modules). A
**signed** module is allowed to do all of it — hence the driver.

```bash
git clone https://github.com/Little-King2022/honor-fan
cd honor-fan/kernel
make
```

### With Secure Boot on (recommended)

```bash
sudo ./sign_and_enroll.sh     # generates a MOK, signs the module, enrolls the key
sudo reboot                   # blue MOK screen: Enroll MOK -> Continue -> Yes -> password
```

Enrolling a MOK does **not** disable Secure Boot. On a dual-boot machine, have your
BitLocker recovery key to hand before changing anything in this area
(`account.microsoft.com/devices/recoverykey`).

Re-run `sign_and_enroll.sh` after each kernel update; the key is reused, so no second
enrollment is needed.

### With Secure Boot off

Nothing to sign — just `sudo insmod honor_fan.ko`.

### Load

```bash
sudo insmod honor_fan.ko                    # monitoring only
sudo insmod honor_fan.ko enable_control=1   # also allow setting speed

# permanently
sudo make install
echo honor_fan | sudo tee /etc/modules-load.d/honor_fan.conf
```

## Usage

```console
$ tools/honor-fan status
mode      : auto (EC firmware)
pwm1      : 255/255
fan1      : 1620 rpm
fan2      : 1320 rpm

CPU       : 56 C
SEN1      : 40 C
...
```

Standard `hwmon`, so `sensors` and anything else that speaks hwmon sees it too.

With `enable_control=1`:

```bash
tools/honor-fan set 70      # manual, 70%
tools/honor-fan auto        # hand back to EC firmware
```

Temperature-driven curve (`tools/fan_curve.py`) with a systemd unit is included. It
returns the fans to firmware control on exit, as does the driver on unload.

### If the fans stop

Unload the module and wait ~40 seconds — the EC recovers by itself:

```bash
sudo rmmod honor_fan; sleep 40; sudo insmod honor_fan.ko
```

The CPU keeps its own hardware thermal protection (throttle and shutdown at 110 °C)
regardless of what this driver does.

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `enable_control` | `0` | Allow writing fan speed. Off = monitoring only. |
| `fsvl_max` | `42` | `FSVL` at `pwm1 = 255`; 42 ≈ 5800 rpm target. |
| `fsud_manual` | `1` | Token written to `FSUD` to select manual mode. |
| `force` | `0` | Skip the DMI model check (other models, at your own risk). |

## Reproducing the research

```bash
sudo apt install acpica-tools
sudo cp /sys/firmware/acpi/tables/DSDT dsdt.dat
iasl -d dsdt.dat
grep -n "RDER\|WTER\|GFNS\|SFCS" dsdt.dsl
```

`tools/ec_watch.sh` diffs the 256-byte EC space over time (needs `ec_sys`), which is how
the temperature probes were located. `tools/fan_probe.sh` walks `FSUD`/`FSVL` and restores
auto on any exit.

## Safety

- `FSVL = 0` is idle, not off; the driver never asks for a zero target.
- The driver hands the fans back to firmware on unload and on `pwm1_enable = 2`.
- The EC can still end up with the fans stopped (see Status). Do not run manual control
  unattended under sustained load until that is fixed.
- Hardware thermal protection is independent of this driver and always active.

## License

GPL-2.0. The driver is a Linux kernel module; the userspace tools are under the same terms.
