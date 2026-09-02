# EC fan protocol — HONOR MagicBook Pro 16 (DRA-XX)

Everything here was recovered from the machine's own ACPI tables (methods `RDER`, `WTER`,
`GFNS`, `SFCS`) except the `FSVL` encoding, which is not statically derivable and was
measured. Firmware excerpts: [ASL-EVIDENCE.md](ASL-EVIDENCE.md).

## 1. The two EC windows

```asl
OperationRegion (ERAM, SystemMemory, 0xFE0B0300, 0x0100)   ; EC RAM mirror (256 B)
OperationRegion (SMA2, SystemMemory, 0xFE0B0400, 0x0080)   ; command mailbox
```

Both are declared `QWordAcc, Lock`. That matters: the ACPI interpreter accesses them with
**64-bit** reads/writes and takes the **ACPI global lock**. A driver must do the same, or
it races the firmware's own AML. `honor_fan` uses 64-bit read-modify-write and
`acpi_acquire_global_lock()`.

### ERAM — the classic 256-byte EC space

Verified by cross-checking against the kernel's thermal zones:

| Offset | Field | Meaning |
|---|---|---|
| `0x20`–`0x29` | `TP00`–`TP09` | temperature probes, °C, signed |
| `0x2B` | `TP0A` | temperature probe |
| `0x1F` | `GDTS` | a *temperature*, signed — not a fan value |
| `0x79` | `ZNCJ` | secondary fan handle; `0xA0` is the firmware default |
| `0x08` | `FNLK`/`FNST` | Fn-lock state |

Mapping confirmed live: `TP00`=CPU/`TCPU`, `TP01`=`SEN2`, `TP03`=`SEN5`, `TP04`=`SEN3`,
`TP05`=`SEN8`, `TP06`=`SEN7`, `TP08`=`SEN6`, `TP09`=`SEN4`, `TP0A`=`SEN1`.

**There is no tachometer in this space.** That is why `ec_sys` alone gets you nowhere.

### SMA2 — the mailbox

| Offset | Name | Meaning |
|---|---|---|
| `0x00` | `CMDB` | command; the EC clears it to 0 on completion |
| `0x01` | `STAT` | status |
| `0x02` | `NUMB` | byte count (always 1 here) |
| `0x03` | `DAT0` | address bits 31:24 |
| `0x04` | `DAT1` | address bits 23:16 |
| `0x05` | `DAT2` | address bits 15:8 |
| `0x06` | `DAT3` | address bits 7:0 |
| `0x07` | `DAT4` | — |
| `0x08` | `DATR` | data in / data out |
| `0x09` | `DAR1` | — |
| `0x0A` | `DAR2` | — |

Commands: `0x80` read, `0x81` write, `0x29` write-unlock.

## 2. RDER — read one extended-EC byte

```
RDER(addr32):
    DAT0..DAT3 = addr >> 24, >> 16, >> 8, >> 0     ; MSB first
    NUMB = 1
    CMDB = 0x80
    poll while CMDB != 0                            ; AML: 256 x Sleep(2ms)
    return (CMDB == 0) ? DATR : 0xFF
```

## 3. WTER — write one extended-EC byte

Note the unlock handshake before every write.

```
WTER(addr32, value):
    DATR = 0x4D                     ; 'M'
    CMDB = 0x29                     ; unlock
    poll while CMDB != 0
    if CMDB != 0: return 0xFF

    DAT0..DAT3 = address bytes, MSB first
    DATR = value
    NUMB = 1
    CMDB = 0x81
    poll while CMDB != 0
    return (CMDB == 0) ? 0 : 0xFF
```

## 4. Fan registers

From `GFNS` ("get fan speed") — this is where we learn there are **two** fans:

| Extended address | Meaning |
|---|---|
| `0x20020A00` / `0x20020A01` | fan 1 rpm, low / high byte |
| `0x20020A02` / `0x20020A03` | fan 2 rpm, low / high byte |
| `0x20020A04` | fan 1 target speed (ASL multiplies by 100) |
| `0x20020A05` | fan 2 target speed |

rpm is little-endian: `lo | (hi << 8)`.

From `SFCS` ("set fan control speed") — the only fan write path in the entire firmware:

| Extended address | ASL name | Meaning |
|---|---|---|
| `0x20020A1B` | `FSUD` | manual-mode selector |
| `0x20020A1C` | `FSVL` | manual speed value |

```asl
Method (SFCS, 1, NotSerialized) {
    CreateByteField (Arg0, 0x02, FSUD)
    CreateWordField (Arg0, 0x03, FSVL)
    WTER (0x20020A1B, FSUD)
    WTER (0x20020A1C, FSVL)
}
```

`FSVL` is declared as a *word* but `WTER` writes `NUMB = 1`, so only the low byte reaches
the EC.

## 5. The FSVL encoding (measured, not derived)

`SFCS` is never invoked with literal constants, so the units had to be measured. Reading
the **target** register `0x20020A04` right after a write is the sensitive instrument — it
updates immediately, whereas the tachometer lags by tens of seconds while the fan ramps.

Writing `FSUD=1, FSVL=v` and reading `0x0A04`:

| FSVL | 10 | 20 | 30 | 40 | 50 | 60 | 80 | 100 |
|---|---|---|---|---|---|---|---|---|
| `0x0A04` | 26 | 36 | 46 | 56 | 66 | 76 | 96 | 116 |
| target rpm | 2600 | 3600 | 4600 | 5600 | 6600 | 7600 | 9600 | 11600 |

Exactly linear, and the intercept equals the idle reading in auto mode (`0x10` = 16 for
fan 1, `0x0E` = 14 for fan 2):

```
target_rpm = (base + FSVL) * 100        base = 16 (fan 1), 14 (fan 2)
```

So `FSVL` is an increment above idle, in units of 100 rpm. `FSVL = 0` gives idle
(~1600 rpm), never off. The fans top out near 5800 rpm, so `FSVL` above ~42 requests a
speed the hardware cannot reach.

## 6. Two behaviours you must handle

### 6.1 Both registers must always be written

Setting `FSUD = 0` alone to return control to the firmware is **not enough**. The stale
`FSVL` value leaves the EC latched with both fans stopped, and it then ignores further
writes. The correct release sequence is:

```
WTER(0x20020A1B, 0)
WTER(0x20020A1C, 0)
```

This was found the hard way: a `FSUD=0`-only release stopped the fans and no combination
of `FSUD` values recovered them until `FSVL` was also zeroed.

### 6.2 The EC desyncs under back-to-back writes

Every write is a full unlock+write handshake. Issued microseconds apart — as a driver
naturally does — the EC stops acknowledging: `0x0A04` reads back 0 and the fans stop. The
same sequence issued from a shell, where each `sudo tee` inserts hundreds of milliseconds,
works reliably.

Recovery is to **stop all mailbox traffic for ~40 seconds**; the EC then resumes normally.
No register write has been found that shortcuts this.

Worse: the EC accepts the *first* manual setting and then stops listening. Writing a new
`FSVL` while `FSUD` is already 1 is swallowed and both fans stop. Routing every change
through a release (`FSUD=0, FSVL=0`) with a 1 s settle before re-arming — which is exactly
the sequence that mapped the encoding reliably from a shell — does **not** fix it inside
the driver.

`honor_fan` paces writes 150 ms apart, skips no-op reprogramming, and verifies the target
read-back, which helps but does not solve it. **This is the main open issue.**

Untested hypotheses:

- a longer or different inter-transaction delay
- an EC query event (`_Qxx`) that `SFCS` triggers indirectly and we bypass
- `ZNCJ` (plain EC `0x79`, default `0xA0`) may need resetting as part of the sequence —
  the firmware's `_QF8` handler does exactly `ECWT (0xA0, RefOf (ZNCJ))`
- a handshake step performed by SMM rather than AML, hence invisible in the tables

## 7. Why userspace cannot do this

With Secure Boot on, the kernel runs `lockdown=integrity`, which blocks:

| Attempt | Result |
|---|---|
| `modprobe ec_sys write_support=1` | `Lockdown: unsafe module parameters is restricted` |
| `/dev/mem` | `Operation not permitted` |
| `ioperm()` on the EC ports | `Operation not permitted` |
| unsigned `insmod` | `Loading of unsigned module is rejected` |

A signed kernel module is permitted to do all of it.
