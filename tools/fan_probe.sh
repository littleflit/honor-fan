#!/usr/bin/env bash
# fan_probe.sh - determine the FSUD/FSVL encoding empirically.
#
# The firmware never calls SFCS with literal constants, so the meaning of
# FSUD (0x20020A1B) and FSVL (0x20020A1C) cannot be read out of the ACPI
# tables.  This walks candidate values and reports the RPM response.
#
# Auto mode is restored on ANY exit, including Ctrl-C.
set -uo pipefail

FSUD=0x20020A1B
FSVL=0x20020A1C
SETTLE=${SETTLE:-6}          # seconds to let the fan react
FAN=${FAN:-fan1}

H=""
for h in /sys/class/hwmon/hwmon*; do
    [ "$(cat "$h/name" 2>/dev/null)" = "honor_fan" ] && H=$h
done
[ -n "$H" ] || { echo "honor_fan module not loaded" >&2; exit 1; }
[ "$EUID" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }

ecw() { echo "$1" > "$H/device/ec_addr"; echo "$2" > "$H/device/ec_data"; }
ecr() { echo "$1" > "$H/device/ec_addr"; cat "$H/device/ec_data"; }
rpm() { cat "$H/${FAN}_input"; }

restore() {
    echo
    echo ">> restoring EC firmware control"
    # BOTH registers must be cleared: a stale FSVL latches the EC with the
    # fans stopped and it then ignores further writes.
    ecw $FSUD 0
    ecw $FSVL 0
    echo 2 > "$H/pwm1_enable" 2>/dev/null || true
}
trap restore EXIT INT TERM

echo "baseline (auto mode):"
echo "  FSUD = $(ecr $FSUD)    FSVL = $(ecr $FSVL)"
sleep $SETTLE
echo "  $FAN = $(rpm) rpm"
echo

echo "=== step 1: which FSUD token enables manual mode? ==="
for u in 1 2 3; do
    ecw $FSUD "$u"; ecw $FSVL 100      # ask for a clearly-high speed
    sleep $SETTLE
    printf "  FSUD=%-3s FSVL=100  -> %s rpm  (readback FSUD=%s FSVL=%s)\n" \
        "$u" "$(rpm)" "$(ecr $FSUD)" "$(ecr $FSVL)"
done
ecw $FSUD 0; ecw $FSVL 0; sleep $SETTLE
echo

echo "=== step 2: FSVL response curve (using FSUD=${FSUD_OK:-1}) ==="
U=${FSUD_OK:-1}
for v in 0 20 40 60 80 100 150 200 255; do
    ecw $FSUD "$U"; ecw $FSVL "$v"
    sleep $SETTLE
    printf "  FSVL=%-4s -> %s rpm\n" "$v" "$(rpm)"
done

echo
echo "Interpretation:"
echo "  * RPM rising up to FSVL=100 then flat  => FSVL is a PERCENT (0-100)"
echo "  * RPM rising all the way to FSVL=255   => FSVL is a DUTY (0-255)"
echo "  * only a few distinct RPM plateaus     => FSVL is a GEAR index"
echo "Set fsvl_max in the module accordingly:  sudo modprobe honor_fan fsvl_max=<max>"
