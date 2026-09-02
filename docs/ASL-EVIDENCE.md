# ASL evidence

Excerpts from this machine's own firmware, decompiled with `iasl -d`.
Reproduce with: `sudo cp /sys/firmware/acpi/tables/DSDT dsdt.dat && iasl -d dsdt.dat`

## The ACPI fans are decoration

Five `PNP0C0B` fans exist (`\_TZ_.FAN0..FAN4`, exposed as `cooling_device2-6`).
Their power resources set flags and call `FNCL()`, which ends here:

```asl
            {
                Local4 = CVF4 /* \_TZ_.CVF4 */
                Release (FMT4)
            }

            \VFN0 = Local0
            \VFN1 = Local1
            \VFN2 = Local2
            \VFN3 = Local3
            \VFN4 = Local4
            \_SB.PC00.LPCB.UPFS (ETMD, Local0, Local1)
        }
```

And `UPFS` is an empty stub — writing `cur_state` does nothing:

```asl
        Method (UPFS, 3, Serialized)
        {
        }
```

## The memory-mapped EC windows

```asl
            OperationRegion (ERAM, SystemMemory, 0xFE0B0300, 0x0100)
            Field (ERAM, QWordAcc, Lock, Preserve)
            {
...
            OperationRegion (SMA2, SystemMemory, 0xFE0B0400, 0x80)
            Field (SMA2, QWordAcc, Lock, Preserve)
            {
                CMDB,   8, 
                STAT,   8, 
                NUMB,   8, 
                DAT0,   8, 
                DAT1,   8, 
                DAT2,   8, 
                DAT3,   8, 
                DAT4,   8, 
                DATR,   8, 
                DAR1,   8, 
                DAR2,   8
            }
```

## RDER — read one extended-EC byte

```asl
            Method (RDER, 1, Serialized)
            {
                Local0 = Acquire (ECMT, 0x07D0)
                If ((Local0 == Zero))
                {
                    Local1 = Arg0
                    Local1 &= 0xFF
                    Local2 = (Arg0 >> 0x08)
                    Local2 &= 0xFF
                    Local3 = (Arg0 >> 0x10)
                    Local3 &= 0xFF
                    Local4 = (Arg0 >> 0x18)
                    Local4 &= 0xFF
                    DAT0 = Local4
                    DAT1 = Local3
                    DAT2 = Local2
                    DAT3 = Local1
                    NUMB = One
                    CMDB = 0x80
                    Local0 = 0x0100
                    While ((Local0 && CMDB))
                    {
                        Sleep (0x02)
                        Local0--
                    }

                    If ((CMDB == Zero))
                    {
                        Local0 = DATR /* \_SB_.PC00.LPCB.H_EC.DATR */
                    }
                    Else
                    {
                        Local0 = 0xFF
                    }

                    Release (ECMT)
                    Return (Local0)
                }

                Release (ECMT)
                Return (0xFF)
            }
```

## WTER — write one extended-EC byte (note the unlock handshake)

```asl
            Method (WTER, 2, Serialized)
            {
                Local0 = Acquire (ECMT, 0x07D0)
                If ((Local0 == Zero))
                {
                    DATR = 0x4D
                    CMDB = 0x29
                    Local0 = 0x00010000
                    While ((Local0 && CMDB))
                    {
                        Sleep (0x02)
                        Local0--
                    }

                    If ((CMDB != Zero))
                    {
                        Local0 = 0xFF
                        Release (ECMT)
                        Return (Local0)
                    }

                    Local1 = Arg0
                    Local1 &= 0xFF
                    Local2 = (Arg0 >> 0x08)
                    Local2 &= 0xFF
                    Local3 = (Arg0 >> 0x10)
                    Local3 &= 0xFF
                    Local4 = (Arg0 >> 0x18)
                    Local4 &= 0xFF
                    DAT0 = Local4
                    DAT1 = Local3
                    DAT2 = Local2
                    DAT3 = Local1
                    DATR = Arg1
                    NUMB = One
                    CMDB = 0x81
                    Local0 = 0x00010000
                    While ((Local0 && CMDB))
                    {
                        Sleep (0x02)
                        Local0--
                    }

                    If ((CMDB == Zero))
                    {
```

## GFNS — where the fan tachometer addresses come from

```asl
    Method (GFNS, 1, Serialized)
    {
        Name (BUFF, Buffer (0x0100){})
        CreateByteField (Arg0, 0x02, FANN)
        CreateByteField (BUFF, Zero, STAT)
        CreateWordField (BUFF, One, FANS)
        CreateWordField (BUFF, 0x03, FNGS)
        STAT = Zero
        FANS = Zero
        Switch (ToInteger (FANN))
        {
            Case (Zero)
            {
                Local1 = (\_SB.PC00.LPCB.H_EC.RDER (0x20020A01) << 0x08)
                Local2 = (\_SB.PC00.LPCB.H_EC.RDER (0x20020A00) | Local1)
                FANS = Local2
                FNGS = (\_SB.PC00.LPCB.H_EC.RDER (0x20020A04) * 0x64)
            }
            Case (One)
            {
                Local1 = (\_SB.PC00.LPCB.H_EC.RDER (0x20020A03) << 0x08)
                Local2 = (\_SB.PC00.LPCB.H_EC.RDER (0x20020A02) | Local1)
                FANS = Local2
                FNGS = (\_SB.PC00.LPCB.H_EC.RDER (0x20020A05) * 0x64)
            }
            Default
            {
                STAT = One
            }

        }

        Return (BUFF) /* \GFNS.BUFF */
```

## SFCS — the only fan write path in the entire firmware

```asl
    Method (SFCS, 1, NotSerialized)
    {
        Name (BUFF, Buffer (0x0100){})
        CreateByteField (BUFF, Zero, STAT)
        CreateByteField (Arg0, 0x02, FSUD)
        CreateWordField (Arg0, 0x03, FSVL)
        STAT = Zero
        \_SB.PC00.LPCB.H_EC.WTER (0x20020A1B, FSUD)
        \_SB.PC00.LPCB.H_EC.WTER (0x20020A1C, FSVL)
        Return (BUFF) /* \SFCS.BUFF */
```
