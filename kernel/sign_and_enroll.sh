#!/usr/bin/env bash
# Generate a Machine Owner Key, sign honor_fan.ko with it, and enroll the key
# so the module loads while Secure Boot stays ON.
#
# Enrolling requires ONE reboot: the blue "MOK Manager" screen appears before
# Ubuntu starts, where you choose "Enroll MOK" -> "Continue" -> "Yes" and type
# the password you set below.
set -euo pipefail

MOKDIR=/var/lib/shim-signed/mok
KEY=$MOKDIR/honor_fan.priv
CRT=$MOKDIR/honor_fan.der
MOD=$(dirname "$(readlink -f "$0")")/honor_fan.ko

[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$MOD" ] || { echo "build the module first: make"; exit 1; }

mkdir -p "$MOKDIR"

if [ ! -f "$KEY" ]; then
    echo ">> generating MOK keypair"
    openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
        -outform DER -keyout "$KEY" -out "$CRT" \
        -subj "/CN=honor_fan module signing key/"
    chmod 600 "$KEY"
else
    echo ">> reusing existing key at $KEY"
fi

echo ">> signing module"
KVER=$(uname -r)
"/usr/src/linux-headers-$KVER/scripts/sign-file" sha256 "$KEY" "$CRT" "$MOD"
modinfo "$MOD" | grep -E "^signer|^sig_id" || true

if mokutil --test-key "$CRT" 2>/dev/null | grep -q "is already enrolled"; then
    echo ">> key already enrolled - you can 'insmod honor_fan.ko' now"
    exit 0
fi

echo
echo ">> enrolling key. You will be asked for a ONE-TIME password;"
echo "   you must retype it at the blue MOK Manager screen after reboot."
mokutil --import "$CRT"

echo
echo "Now reboot. At the blue screen choose:"
echo "   Enroll MOK -> Continue -> Yes -> <the password you just set> -> Reboot"
