#!/usr/bin/env bash
set -euo pipefail

# PocketBeagle 2 Rev A1 (PB2VA1 / TI AM6254A) environment doctor script.
# Run either directly on the board over SSH or via: PocketBeagle2/scripts/pb2.ps1 -Action doctor

echo "=== PB2 A1 (AM6254A) Doctor ==="
echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

echo "--- Architecture & Kernel ---"
uname -a
if [ -f /etc/os-release ]; then
  grep -E '^(PRETTY_NAME|VERSION_CODENAME)=' /etc/os-release || true
fi

echo "--- Board Model, SoC, CPU Cores & EEPROM ---"
if [ -r /proc/device-tree/model ]; then
  echo -n "Model: "; tr -d '\0' < /proc/device-tree/model; echo ""
else
  echo "Model: UNAVAILABLE"
fi
if [ -r /proc/device-tree/compatible ]; then
  echo -n "Compatible: "; tr '\0' ' ' < /proc/device-tree/compatible; echo ""
fi
if [ -r /sys/devices/soc0/soc_id ]; then
  echo "SoC ID: $(cat /sys/devices/soc0/soc_id 2>/dev/null || echo unknown)"
fi
CPUS="$(nproc 2>/dev/null || echo 0)"
echo "CPU Cores: $CPUS (expected 4 on A1 AM6254A, 2 on legacy A0 AM6232)"
for ee in /sys/bus/i2c/devices/*/eeprom; do
  [ -r "$ee" ] && echo "EEPROM node: $ee"
done

ARCH="$(uname -m)"
if [ "$ARCH" != "aarch64" ]; then
  echo "WARNING: Expected aarch64 on PocketBeagle 2 A1, got $ARCH"
fi

echo "--- Memory & Storage ---"
free -h
lsblk -o NAME,SIZE,TYPE,MOUNTPOINT | head -n 20
df -h / /boot/firmware 2>/dev/null || df -h /

echo "--- Build & Debug Tools ---"
for tool in cmake ninja gcc g++ gdb gdbserver clpru; do
  if command -v "$tool" >/dev/null 2>&1; then
    printf "  %-10s : %s\n" "$tool" "$(command -v "$tool")"
  else
    printf "  %-10s : MISSING\n" "$tool"
  fi
done

echo "--- PRU Remoteproc Discovery (by name, never hardcoded index) ---"
if [ -d /sys/class/remoteproc ]; then
  found_rp=0
  for rp in /sys/class/remoteproc/remoteproc*; do
    [ -e "$rp" ] || continue
    found_rp=1
    name="$(cat "$rp/name" 2>/dev/null || echo unknown)"
    state="$(cat "$rp/state" 2>/dev/null || echo unknown)"
    fw="$(cat "$rp/firmware" 2>/dev/null || echo none)"
    echo "  $(basename "$rp"): name=$name state=$state firmware=$fw"
  done
  if [ "$found_rp" -eq 0 ]; then
    echo "  No remoteproc entries found."
  fi
else
  echo "  /sys/class/remoteproc not present."
fi

echo "--- UART Devices ---"
ls -l /dev/ttyS* /dev/ttyAMA* 2>/dev/null || echo "  No ttyS/ttyAMA devices listed."

echo "--- Kernel Errors (dmesg) ---"
dmesg -T --level=err,crit,alert,emerg 2>/dev/null | tail -n 15 || dmesg | grep -iE 'error|fail|pru|remoteproc' | tail -n 15 || true

echo "=== PB2 A0 Doctor Done ==="
