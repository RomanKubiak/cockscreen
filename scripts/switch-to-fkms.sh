#!/usr/bin/env bash
# Switches the Pi from vc4-kms-v3d to vc4-fkms-v3d for composite GL support.
# Run from the dev machine: bash scripts/switch-to-fkms.sh
# The Pi will need a reboot after this script completes.
set -euo pipefail

PI=atom@192.168.41.181

echo "=== Patching /boot/firmware/config.txt ==="
ssh "$PI" "sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.bak-kms"
ssh "$PI" "sudo sed -i \
  -e 's/dtoverlay=vc4-kms-v3d,composite/dtoverlay=vc4-fkms-v3d/' \
  -e '/^disable_fw_kms_setup/d' \
  /boot/firmware/config.txt"
echo "config.txt result:"
ssh "$PI" "grep -E 'dtoverlay|disable_fw_kms|max_framebuffers|enable_tvout|sdtv' /boot/firmware/config.txt"

echo
echo "=== Patching /boot/firmware/cmdline.txt ==="
# Remove video= kernel args — fkms display mode is set by the firmware, not
# kernel video= params.  Keep everything else on one line.
ssh "$PI" "sudo cp /boot/firmware/cmdline.txt /boot/firmware/cmdline.txt.bak-kms"
ssh "$PI" "sudo sed -i 's/ video=HDMI-A-1:[^ ]*//g; s/ video=Composite-1:[^ ]*//g' /boot/firmware/cmdline.txt"
echo "cmdline.txt result:"
ssh "$PI" "cat /boot/firmware/cmdline.txt"

echo
echo "=== Done. Reboot the Pi to activate fkms: ==="
echo "  ssh $PI 'sudo reboot'"
