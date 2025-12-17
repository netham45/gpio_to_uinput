# gpio_to_uinput

`gpio_to_uinput` turns a range of Linux GPIO lines (using the chardev v2 API) into a virtual gamepad plus an optional virtual keyboard exposed through `uinput`. This lets inexpensive buttons, switches or encoder-style inputs show up as regular input devices on Linux and Android (typically requires root on Android).

Key features:

- Requests every GPIO line in a chosen `[start, end]` range as pull-up inputs with both-edge interrupts and optional kernel-level debounce.
- Applies an additional userspace debounce window so noisy buttons do not spam events.
- Supports per-GPIO mappings to gamepad buttons, hat directions, or keyboard keys via a simple text file, with automatic assignments for unmapped lines.
- Creates up to two virtual devices (`gpio-virtual-gamepad`, `gpio-virtual-keyboard`) so hotkeys can be routed independently of face buttons.

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
<gpio_offset> <token>
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

## Behavior

- **Active level:** inputs are active-low by default (falling edge = press). Use `--active-high` for active-high wiring.
- **Debounce:** `--debounce-us` configures both a kernel attribute (if supported) and a userspace filter that drops events that arrive faster than the specified interval.
- **Exclusions:** GPIO offset `36` is skipped automatically because it tends to be noisy on Raspberry Pi boards.
- **Logging:** Every accepted edge is logged with timestamp, GPIO number, and the resolved action, which helps tune debounce windows and wiring.

## Troubleshooting

- If no lines can be requested, verify permissions on `/dev/gpiochip*` and make sure no other process owns the pins.
- Ensure `/dev/uinput` exists and is writable; load the `uinput` kernel module if needed.
- Use `--list-options` to see every button/key token the parser recognizes when building your mapping file.
