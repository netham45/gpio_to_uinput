#!/bin/bash
set -euo pipefail

echo "Verify that the requested version matches what the system has"

upver="$(uname -r | cut -d. -f 1,2)"
hdr="/usr/src/linux-headers-${upver}rpt-rpi-2712/"

echo "Does $hdr - $upver == $(uname -r) ?"

sudo mkdir -p "$hdr/drivers/power/supply"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$tmpdir"
sudo apt-get install -y wget xz-utils

wget -O "linux-$upver.tar.xz" "https://www.kernel.org/pub/linux/kernel/v6.x/linux-$upver.tar.xz"
tar -xf "linux-$upver.tar.xz" "linux-$upver/drivers/power/supply/test_power.c"

sudo install -m 0644 "linux-$upver/drivers/power/supply/test_power.c" \
  "$hdr/drivers/power/supply/test_power.c"

ls -l "$hdr/drivers/power/supply/test_power.c"
