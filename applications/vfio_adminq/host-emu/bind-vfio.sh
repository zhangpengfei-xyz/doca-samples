#!/bin/sh
set -eu

BDF="${1:-0000:78:00.0}"
case "$BDF" in
    [0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]:*)
        ;;
    *)
        BDF="0000:$BDF"
        ;;
esac

DEV="/sys/bus/pci/devices/$BDF"
if [ ! -e "$DEV" ]; then
    echo "missing PCI device $BDF" >&2
    exit 1
fi

modprobe vfio-pci

if [ -w /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts ]; then
    if [ "$(cat /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts)" = "N" ]; then
        echo "enable vfio_iommu_type1 allow_unsafe_interrupts for this demo"
        echo 1 > /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts
    fi
fi

echo vfio-pci > "$DEV/driver_override"
if [ -L "$DEV/driver" ]; then
    OLD_DRIVER="$(basename "$(readlink "$DEV/driver")")"
    echo "unbind $BDF from $OLD_DRIVER"
    echo "$BDF" > "$DEV/driver/unbind"
fi

echo "bind $BDF to vfio-pci"
echo "$BDF" > /sys/bus/pci/drivers/vfio-pci/bind

DRIVER="$(basename "$(readlink "$DEV/driver")")"
GROUP="$(basename "$(readlink "$DEV/iommu_group")")"
echo "bound $BDF to $DRIVER, iommu_group=$GROUP"
ls -l "/dev/vfio/$GROUP"
