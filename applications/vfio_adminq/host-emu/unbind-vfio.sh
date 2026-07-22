#!/usr/bin/env bash
set -euo pipefail

BDF="${1:-}"
if [[ -z "$BDF" ]]; then
    echo "usage: $0 <domain:bus:device.function>" >&2
    exit 2
fi

DEV="/sys/bus/pci/devices/$BDF"
if [[ ! -d "$DEV" ]]; then
    echo "$BDF is already absent"
    exit 0
fi

if [[ -L "$DEV/driver" ]]; then
    DRIVER="$(basename "$(readlink "$DEV/driver")")"
    echo "unbind $BDF from $DRIVER"
    echo "$BDF" > "$DEV/driver/unbind"
fi

# Remove the Host PCI object while the emulated endpoint still exists.  The
# DPU side may destroy the representor only after this operation completes.
echo 1 > "$DEV/remove"
echo "removed Host PCI function $BDF"
