#!/usr/bin/env python3
"""
fan_curve.py - temperature-driven fan curve for HONOR MagicBook Pro 16.

Reads the hottest relevant EC probe and drives pwm1 on the honor_fan hwmon
device.  Hysteresis avoids audible hunting.  If anything goes wrong -- an
exception, a signal, a read failure -- the fans are handed straight back to
EC firmware control, which is always the safe state.
"""
import os
import signal
import sys
import time

# (temperature >= C, fan percent).  Evaluated top-down, so keep it descending.
CURVE = [
    (90, 100),
    (85,  90),
    (80,  80),
    (75,  65),
    (70,  50),
    (60,  35),
    (50,  25),
    (0,    0),
]

HYSTERESIS = 4      # deg C you must fall below before stepping down
INTERVAL   = 3      # seconds
PANIC_TEMP = 95     # above this, always 100% regardless of curve


def find_hwmon():
    for entry in sorted(os.listdir("/sys/class/hwmon")):
        base = f"/sys/class/hwmon/{entry}"
        try:
            with open(f"{base}/name") as fh:
                if fh.read().strip() == "honor_fan":
                    return base
        except OSError:
            continue
    sys.exit("honor_fan hwmon not found - is the module loaded?")


class Fan:
    def __init__(self, base):
        self.base = base

    def _write(self, attr, val):
        with open(f"{self.base}/{attr}", "w") as fh:
            fh.write(str(val))

    def read_int(self, attr):
        with open(f"{self.base}/{attr}") as fh:
            return int(fh.read().strip())

    def hottest(self):
        """Max over the CPU/board probes, in whole degrees C."""
        temps = []
        for n in range(1, 12):
            try:
                temps.append(self.read_int(f"temp{n}_input") // 1000)
            except (OSError, ValueError):
                pass
        if not temps:
            raise RuntimeError("no temperature probes readable")
        return max(temps)

    def set_percent(self, pct):
        self._write("pwm1_enable", 1)
        self._write("pwm1", pct * 255 // 100)

    def auto(self):
        self._write("pwm1_enable", 2)


def target_for(temp, current):
    if temp >= PANIC_TEMP:
        return 100
    for threshold, pct in CURVE:
        if temp >= threshold:
            # only step down once we are clearly below the current band
            if pct < current and temp > threshold - HYSTERESIS:
                return current
            return pct
    return 0


def main():
    fan = Fan(find_hwmon())
    current = 0
    stop = False

    def handler(signum, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, handler)
    signal.signal(signal.SIGINT, handler)

    try:
        while not stop:
            temp = fan.hottest()
            want = target_for(temp, current)
            if want != current:
                fan.set_percent(want)
                current = want
                print(f"{temp}C -> fan {want}%", flush=True)
            time.sleep(INTERVAL)
    finally:
        # always give the hardware back to the firmware
        try:
            fan.auto()
            print("restored EC firmware control", flush=True)
        except OSError:
            pass


if __name__ == "__main__":
    main()
