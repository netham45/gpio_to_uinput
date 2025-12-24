# raspberrypios_battery

This directory turns a Raspberry Pi OS system into something that looks battery
powered by reusing the in-tree `test_power` driver and feeding it data from the
same Arduino-style I2C fuel gauge used by `gpio_to_uinput`. Once the DKMS module
is installed, `update_test_power.sh` reads voltage/SOC over I2C and writes the
values to `/sys/class/power_supply/test_battery` so the desktop shell (or other
status bars) shows a battery meter.

## Contents

- `get_kernel_source.sh` &mdash; downloads the upstream kernel tarball that matches
  the running major.minor version and copies `drivers/power/supply/test_power.c`
  into `/usr/src/linux-headers-<version>rpt-rpi-2712/`.
- `install_test_power_dkms.sh` &mdash; wraps that source file in a tiny DKMS tree,
  builds it against the active kernel, and installs the module so it survives
  kernel updates.
- `update_test_power.sh` &mdash; polls I2C register `0x09` (millivolts) and
  `0x0D` (optional SOC) from the Arduino and updates `test_power`'s parameters.

## Requirements

- Raspberry Pi OS (Bookworm or newer) with kernel headers that match `uname -r`.
- Root access for DKMS installation, module loading, and writing module parameters.
- `i2c-tools`, `dkms`, `build-essential`, `wget`, and `xz-utils`.
- A co-processor that responds to the register layout described above at 7-bit
  address `0x42` on I2C bus `1` (edit the scripts if yours differs).

## Workflow

1. **Copy the reference `test_power.c`:**

   ```bash
   ./get_kernel_source.sh
   ```

   The script sanity-checks that `/usr/src/linux-headers-<major.minor>rpt-rpi-2712`
   matches the running kernel and then pulls only `drivers/power/supply/test_power.c`
   from kernel.org. Use this step primarily on Pi 5 images where the packaged
   headers may omit the file.

2. **Install the DKMS module:**

   ```bash
   sudo ./install_test_power_dkms.sh        # auto-detects $(uname -r)
   # or, for a custom kernel:
   sudo ./install_test_power_dkms.sh 6.6.51+rpt-rpi-2712
   ```

   The script copies `test_power.c` into `/usr/src/test_power-0.1/`, generates a
   DKMS `Makefile`/`dkms.conf`, removes any previous build, and then performs the
   usual `dkms add/build/install` cycle. On success `modprobe test_power` should
   expose `/sys/class/power_supply/test_battery`.

3. **Feed the module with real readings:**

   ```bash
   sudo ./update_test_power.sh
   ```

   Run this on a schedule (systemd service/timer, cron job, or a simple loop) to
   keep the reported capacity fresh. The script enforces root, ensures
   `test_power` is loaded, reads the Arduino via `i2ctransfer`, and populates
   `/sys/module/test_power/parameters/{battery_capacity,battery_voltage}`.

## Customization

Edit the variables at the top of `update_test_power.sh` to match your hardware:

- `BUS` / `ADDR` choose the I2C bus and 7-bit address.
- `V_EMPTY` / `V_FULL` define the fallback voltage-to-percentage mapping for
  boards that do not supply the `0x0D` SOC register.

The DKMS script accepts an optional kernel version argument if you want to build
for a different kernel (e.g., after installing a new Pi kernel but before
rebooting).

## Troubleshooting

- `install_test_power_dkms.sh` aborts with “Couldn't find test_power.c”: make sure
  the headers that correspond to `uname -r` are installed or run
  `get_kernel_source.sh` to stage the file under `/usr/src`.
- Re-run the DKMS script whenever you upgrade kernels so the module is built for
  the new `uname -r`.
- If `/sys/class/power_supply/test_battery` never appears, check `dmesg | tail -n
  50` after running `modprobe test_power` for compiler or symbol errors.
- For I2C read failures, confirm the bus/address with `i2cdetect -y 1` and verify
  that the Arduino sketch matches the register layout expected here.
