#!/usr/bin/env bash
set -euo pipefail

MODNAME="test_power"
MODVER="0.1"
KVER="${1:-$(uname -r)}"
SRCDIR="/usr/src/${MODNAME}-${MODVER}"

if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo $0 [kernel_version]"
  exit 1
fi

echo "[*] Installing build deps + headers (Pi OS)..."
apt-get update -y
apt-get install -y dkms build-essential

echo "[*] Looking for test_power.c in installed kernel headers/source for: ${KVER}"
candidates=(
  "/lib/modules/${KVER}/source/drivers/power/supply/test_power.c"
  "/lib/modules/${KVER}/build/drivers/power/supply/test_power.c"
  "/usr/src/linux-headers-${KVER}/drivers/power/supply/test_power.c"
  "/usr/src/kernel/drivers/power/supply/test_power.c"
)
src=""
for p in "${candidates[@]}"; do
  if [[ -f "$p" ]]; then
    src="$p"
    break
  fi
done

if [[ -z "$src" ]]; then
  echo "ERROR: Couldn't find test_power.c in your installed headers/source."
  echo "Tried:"
  printf '  - %s\n' "${candidates[@]}"
  echo
  echo "Sanity checks:"
  echo "  uname -r                  -> ${KVER}"
  echo "  dpkg -l raspberrypi-kernel-headers"
  echo
  echo "If headers are installed but these paths don't exist on your image,"
  echo "you'll need a kernel source tree that matches ${KVER} to pull test_power.c from."
  exit 2
fi

echo "[*] Using source: ${src}"

echo "[*] Preparing DKMS source tree: ${SRCDIR}"
rm -rf "${SRCDIR}"
mkdir -p "${SRCDIR}"
cp -v "${src}" "${SRCDIR}/test_power.c"

cat > "${SRCDIR}/Makefile" <<'MAKE'
obj-m := test_power.o

all:
        $(MAKE) -C /lib/modules/$(KVER)/build M=$(PWD) modules

clean:
        $(MAKE) -C /lib/modules/$(KVER)/build M=$(PWD) clean
MAKE

cat > "${SRCDIR}/dkms.conf" <<DKMS
PACKAGE_NAME="${MODNAME}"
PACKAGE_VERSION="${MODVER}"

BUILT_MODULE_NAME[0]="${MODNAME}"
BUILT_MODULE_LOCATION[0]="."
DEST_MODULE_LOCATION[0]="/updates/dkms"

MAKE[0]="make KVER=\${kernelver}"
CLEAN="make clean KVER=\${kernelver}"

AUTOINSTALL="yes"
DKMS

echo "[*] Removing any previous DKMS install of ${MODNAME}/${MODVER} (if present)..."
dkms remove -m "${MODNAME}" -v "${MODVER}" --all >/dev/null 2>&1 || true

echo "[*] DKMS add/build/install for kernel: ${KVER}"
dkms add -m "${MODNAME}" -v "${MODVER}"
dkms build -m "${MODNAME}" -v "${MODVER}" -k "${KVER}"
dkms install -m "${MODNAME}" -v "${MODVER}" -k "${KVER}"

echo "[*] depmod..."
depmod -a "${KVER}"

echo "[*] Installed. Module info:"
modinfo "${MODNAME}" | sed -n '1,20p' || true

echo "[*] Attempting to load module (optional)..."
modprobe "${MODNAME}" || true

echo "[*] Power supply entries now:"
ls -1 /sys/class/power_supply || true

echo
echo "If you see /sys/class/power_supply/test_battery you can set percent like:"
echo "  echo 73 | sudo tee /sys/module/test_power/parameters/battery_capacity"

