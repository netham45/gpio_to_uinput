# batocera_battery

`update_virtual_battery.sh` pretends there is a Li-ion pack attached to Batocera /
EmulationStation by bind-mounting a fake `/sys/class/power_supply` tree that exports
`BAT0`. The script polls an Arduino-style I2C co-processor (default address
`0x42` on bus `1`) that serves the battery voltage (`REG_VBAT_MV = 0x09`) and,
optionally, a state-of-charge register (`REG_SOC = 0x0D`). If the SOC register is
not implemented the script derives the percentage from a LUT-based voltage curve
and smooths the readings before exposing them to Batocera.

## Requirements

- Batocera or another distro where you can mount-bind over `/sys/class/power_supply`.
- Root access so the script can read I2C, create the fake power_supply tree, and
  mount/umount the bind overlay.
- `i2ctransfer` from `i2c-tools`.
- A companion microcontroller that responds to the registers listed above.

## Configuration

Tweak the variables near the top of the script if your hardware differs:

| Variable | Purpose |
| --- | --- |
| `BUS` / `ADDR` | I2C bus number and 7-bit address of the co-processor. |
| `REG_VBAT_MV`, `REG_SOC` | Register offsets for millivolt and SOC readings. |
| `SMOOTH_ALPHA`, `ENABLE_SMOOTHING` | Configure exponential smoothing to tame sag/spikes. |
| `SYS_PS`, `FAKE_ROOT` | Paths for the fake power_supply tree (rarely need changing). |

The script keeps a copy of the original `/sys/class/power_supply` mount under
`/tmp/sysps_real` and mirrors non-`BAT0` entries into the fake tree so AC adapters
or other sensors remain visible.

## Usage

1. Copy `update_virtual_battery.sh` somewhere persistent (e.g. `/userdata/system/`)
   and make it executable: `chmod +x update_virtual_battery.sh`.
2. Invoke it as root whenever you want to refresh the readings:

   ```bash
   sudo ./update_virtual_battery.sh update
   ```

   A systemd service, cron job, or a `while true; sleep 5; ...; done` loop inside
   `custom.sh` works well to keep Batocera updated.

The script also exposes two helper modes:

- `sudo ./update_virtual_battery.sh status` &rarr; show whether the fake overlay is
  mounted and print the current capacity/status fields.
- `sudo ./update_virtual_battery.sh stop` &rarr; unmount both bind mounts and clean
  up the temporary directories.

Running `update` automatically ensures the fake tree exists, reads I2C, optionally
filters the SOC value, and writes `capacity`, `status`, and `uevent` in
`/sys/class/power_supply/BAT0`. When SOC reaches 100% the script reports `Full`,
otherwise it sticks to `Discharging`.

## Troubleshooting

- `i2ctransfer` errors usually mean the Arduino is not connected or `BUS`/`ADDR`
  is wrong. Test with `i2cdetect -y <bus>`.
- If Batocera never sees the fake battery, check `/sys/class/power_supply` for
  `.batocera_fake_power_supply`. Delete `/tmp/power_supply` and rerun the script
  to rebuild the overlay.
- Voltage-only setups may show noisy percentages; reduce `SMOOTH_ALPHA` for a
  steadier reading or disable smoothing entirely by setting `ENABLE_SMOOTHING=0`.
