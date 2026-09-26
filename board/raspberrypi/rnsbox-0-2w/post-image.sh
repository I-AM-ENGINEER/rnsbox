#!/bin/bash
# RNSBox post-image: ship the usb.* gadget flag files on the boot FAT (same
# /boot flag-file semantics as the LicheeRV Nano-e build — /etc/init.d/
# S10usbdev composes the gadget from them), pin our kernel cmdline, then run
# the stock Raspberry Pi post-image (assembles boot.vfat + sdcard.img).
#
# The boot FAT is mounted at /boot by fstab, so the flags are visible to the
# init scripts (and editable from any PC).
set -e

BOARD_DIR="$(dirname "$0")"
FW="${BINARIES_DIR}/rpi-firmware"

# CDC-NCM gadget identity (read at boot by S10usbdev). Windows caches the
# MS-OS descriptor by (VID, PID, bcdDevice) — bump bcdDevice whenever the
# gadget topology changes.
: > "${FW}/usb.dev"
: > "${FW}/usb.ncm"
echo 0x1d6b                   > "${FW}/usb.idVendor"
echo 0x0105                   > "${FW}/usb.idProduct"
echo 0x0105                   > "${FW}/usb.bcdDevice"
echo "RNSBox"                 > "${FW}/usb.manufacturer"
echo "RNSBox Reticulum router (Pi Zero 2 W)" > "${FW}/usb.product"
echo "02:1d:6b:01:04:02"      > "${FW}/usb.ncm.mac"
echo "rnsbox"                 > "${FW}/hostname"
# usb.disk0 (+ .size) for the read-only clients DVD is added by
# build-rnsbox.sh for the dvd variant; lite ships NCM-only.

# Our kernel cmdline (no serial console — UART0 belongs to SLIP).
cat "${BOARD_DIR}/cmdline.txt" > "${FW}/cmdline.txt"

exec "${BOARD_DIR}/../post-image.sh"
