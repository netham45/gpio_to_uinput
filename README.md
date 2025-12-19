# gpio_to_uinput

`gpio_to_uinput` turns a range of Linux GPIO lines (using the chardev v2 API) into a virtual gamepad plus an optional virtual keyboard exposed through `uinput`. This lets inexpensive buttons, switches or encoder-style inputs show up as regular input devices on Linux and Android (typically requires root on Android).

Key features:

- Requests every GPIO line in a chosen `[start, end]` range as pull-up inputs with both-edge interrupts and optional kernel-level debounce.
- Applies an additional userspace debounce window so noisy buttons do not spam events.
- Supports per-GPIO mappings to gamepad buttons, hat directions, or keyboard keys via a simple text file, with automatic assignments for unmapped lines.
- Creates up to two virtual devices (`gpio-virtual-gamepad`, `gpio-virtual-keyboard`) so hotkeys can be routed independently of face buttons.
- Optionally polls an Arduino-style I2C slave (A0/A1 -> left stick, A2/A3 -> right stick, A6 -> slider) and exposes its D2..D13 digital lines as additional mappable inputs.

## Requirements

- Linux/Android kernel with GPIO chardev v2 (`/dev/gpiochipN`) and `uinput` enabled.
- Access rights to read the target GPIO chip and write to `/dev/uinput` (root on most systems).
- `g++` with C++17 support to compile the tool.

## Build

```bash
./build.sh
# or
g++ -O2 -std=c++17 gpio_to_uinput.cpp -o gpio_to_uinput
```

Copy the resulting binary (and map file) to your target if you compile on a different machine.

## Usage

```
gpio_to_uinput [--chip /dev/gpiochipN] [--start N] [--end N]
               [--debounce-us N] [--event-buf N] [--map path] [--active-high]
               [--i2c-dev /dev/i2c-X] [--i2c-addr 0x42] [--i2c-interval-ms N]
               [--i2c-log] [--i2c-no-axes]
               [--auto buttons|keys|none] [--list-options]
```

Typical Android invocation (must run as root):

```bash
su -c /data/local/tmp/gpio_to_uinput \
  --chip /dev/gpiochip0 \
  --start 2 --end 27 \
  --map /data/local/tmp/gpio.map \
  --debounce-us 10000
```

Useful tooling:

- `evtest` (Linux) or `getevent -lp` (Android) to inspect the generated virtual devices.
- `gpioinfo` from `libgpiod` can confirm which lines are available.

## Mapping file

Each non-comment line assigns a GPIO offset to an action:

```
<gpio_offset|D2..D13|I2C:Dn> <token>
```

Tokens support:

- `HAT_UP`, `HAT_DOWN`, `HAT_LEFT`, `HAT_RIGHT` for a d-pad style hat switch.
- `BTN_*` names from Linux's `input-event-codes.h` (see `--list-options` for the exact subset).
- `KEY_*` names and aliases (`ENTER`, `SPACE`, `A`, `F1`, numeric EV_KEY codes, etc.) that go to the keyboard device.

`example.map` demonstrates a complete setup with a hat d-pad, gamepad buttons, volume keys, and a hotkey. Colons can be used as separators (`15:HAT_UP`) but are normalized to spaces internally. Lines referencing GPIOs outside the requested range are ignored.

When `--map` is omitted, a minimal default mapping is synthesized (hat + A button). Unmapped lines can be auto-filled:

- `--auto buttons` (default) cycles through common `BTN_*` codes.
- `--auto keys` emits keyboard keycodes instead.
- `--auto none` leaves unmapped lines inactive.

When the I2C poller is enabled (see below), `D2`..`D13` (or `I2C:D2` style prefixes) in the first column refer to bits inside the Arduino-provided digital mask. These pins use the same tokens and active level rules as native GPIO lines, so you can map `D2 BTN_SOUTH` to treat Arduino D2 as a gamepad button, or point them at hat directions/keyboard keys.

## I2C co-processor mode

Passing `--i2c-dev /dev/i2c-1` (and optionally `--i2c-addr 0x42 --i2c-interval-ms 5`) enables polling of the companion Arduino sketch described above. Every poll reads six 16-bit values: A0, A1, A2, A3, A6 and a digital bitmask. The analog channels are converted into five ABS axes:

- A0 → `ABS_X` (left stick X)
- A1 → `ABS_Y` (left stick Y)
- A2 → `ABS_RX` (right stick X)
- A3 → `ABS_RY` (right stick Y)
- A6 → `ABS_Z` (auxiliary slider)

Each axis auto-calibrates itself by tracking the highest raw value seen and scaling the current reading into the `0..100` range, so sticks never need manual calibration—just sweep them once through their travel.

The final 16-bit word in the payload exposes the instantaneous D2..D13 pin levels. Use the `D#` identifiers described above to map those pins to any action token. The `--active-high` flag affects both physical GPIOs and these I2C-backed pins (by default a logical low means “pressed” to match pull-up wiring).

Because the I2C poller feeds a single virtual gamepad alongside the GPIO-driven buttons, you can mix and match physical Raspberry Pi pins with Arduino-provided sticks/buttons in one map file. Prefer to ignore the analog channels and use only the digital mask? Pass `--i2c-no-axes` and no ABS axes will be registered. Need to debug the Arduino payload? Add `--i2c-log` and every poll dumps the raw 16-bit readings (`i2c_raw=... dmask=0x...`) before scaling or mapping so you can verify wiring and calibration.

## Behavior

- **Active level:** inputs are active-low by default (falling edge = press). Use `--active-high` for active-high wiring.
- **Debounce:** `--debounce-us` configures both a kernel attribute (if supported) and a userspace filter that drops events that arrive faster than the specified interval.
- **Exclusions:** GPIO offset `36` is skipped automatically because it tends to be noisy on Raspberry Pi boards.
- **Logging:** Every accepted edge is logged with timestamp, GPIO number, and the resolved action, which helps tune debounce windows and wiring.

## Troubleshooting

- If no lines can be requested, verify permissions on `/dev/gpiochip*` and make sure no other process owns the pins.
- Ensure `/dev/uinput` exists and is writable; load the `uinput` kernel module if needed.
- Use `--list-options` to see every button/key token the parser recognizes when building your mapping file.
