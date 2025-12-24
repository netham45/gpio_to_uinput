#!/bin/bash
set -euo pipefail

## This application creates a virtual battery that batocera/emulationstation can query by mount binding over /sys/class/power_supply and providing a fake battery object

# ---------------- I2C source ----------------
BUS=1
ADDR=0x42

REG_VBAT_MV=0x09   # uint16 LE, millivolts
REG_SOC=0x0D       # optional uint16 LE, 0..100

# ---------------- Fake power_supply via bind mount ----------------
SYS_PS="/sys/class/power_supply"
FAKE_ROOT="/tmp/power_supply"
FAKE_BAT="$FAKE_ROOT/BAT0"
REAL_SNAPSHOT="/tmp/sysps_real"

MARKER_NAME=".batocera_fake_power_supply"
MARKER_CONTENT="batocera-fake-ps-v1"

# Smoothing (helps with voltage sag / load spikes)
SMOOTH_FILE="/tmp/bat0_soc_last"
SMOOTH_ALPHA="0.25"   # 0..1, higher = more responsive, lower = steadier
ENABLE_SMOOTHING=1    # set to 0 to disable

die(){ echo "error: $*" >&2; exit 1; }

hexbyte_to_dec() {
  local t="${1#0x}"; t="${t#0X}"
  echo $((16#$t))
}

read_word_le() {
  local cmd="$1"
  local out b0 b1 lo hi
  out="$(i2ctransfer -y "$BUS" "w1@${ADDR}" "$cmd" "r2")" || return 1
  read -r b0 b1 <<<"$out"
  lo="$(hexbyte_to_dec "$b0")"
  hi="$(hexbyte_to_dec "$b1")"
  echo $((hi*256 + lo))
}

is_mounted() {
  local mnt="$1"
  awk -v m="$mnt" '$2==m{exit 0} END{exit 1}' /proc/mounts
}

is_fake_active() {
  [[ -f "$SYS_PS/$MARKER_NAME" ]] && grep -qx "$MARKER_CONTENT" "$SYS_PS/$MARKER_NAME"
}

safe_empty_dir() {
  local d="$1"
  mkdir -p "$d"
  rm -rf "$d"/* "$d"/.[!.]* "$d"/..?* 2>/dev/null || true
}

ensure_virtual_battery() {
  if is_fake_active; then
    mkdir -p "$FAKE_BAT"
    return 0
  fi

  mkdir -p "$REAL_SNAPSHOT"
  if ! is_mounted "$REAL_SNAPSHOT"; then
    mount -o bind "$SYS_PS" "$REAL_SNAPSHOT" || die "failed to bind-mount $SYS_PS -> $REAL_SNAPSHOT"
  fi

  safe_empty_dir "$FAKE_ROOT"
  printf "%s\n" "$MARKER_CONTENT" > "$FAKE_ROOT/$MARKER_NAME"

  # Preserve existing entries (AC, etc.)
  shopt -s nullglob
  for x in "$REAL_SNAPSHOT"/*; do
    name="$(basename "$x")"
    [[ "$name" == "BAT0" ]] && continue
    ln -s "$x" "$FAKE_ROOT/$name" 2>/dev/null || true
  done
  shopt -u nullglob

  mkdir -p "$FAKE_BAT"
  printf "Battery\n"     > "$FAKE_BAT/type"
  printf "1\n"           > "$FAKE_BAT/present"
  printf "Discharging\n" > "$FAKE_BAT/status"
  printf "0\n"           > "$FAKE_BAT/capacity"
  printf "POWER_SUPPLY_NAME=BAT0\nPOWER_SUPPLY_TYPE=Battery\n" > "$FAKE_BAT/uevent"
  chmod 755 "$FAKE_BAT"
  chmod 644 "$FAKE_BAT/"*

  mount -o bind "$FAKE_ROOT" "$SYS_PS" || die "failed to bind-mount $FAKE_ROOT -> $SYS_PS"
  is_fake_active || die "bind mount happened but marker not visible at $SYS_PS (unexpected)"
}

# ---- Curved voltage->SOC (LUT + linear interpolation between points) ----
# These points are a decent "normal" single-cell Li-ion OCV-ish curve.
# Under load you’ll read lower voltage; smoothing + this curve usually feels right.
soc_from_voltage_curve() {
  local v="$1"
  awk -v v="$v" '
    function clamp(x, lo, hi){ return (x<lo?lo:(x>hi?hi:x)) }
    BEGIN{
      # Voltage (V) , SOC (%)
      n=0
      n++; V[n]=3.00; S[n]=0
      n++; V[n]=3.30; S[n]=5
      n++; V[n]=3.50; S[n]=10
      n++; V[n]=3.60; S[n]=20
      n++; V[n]=3.65; S[n]=25
      n++; V[n]=3.70; S[n]=35
      n++; V[n]=3.75; S[n]=45
      n++; V[n]=3.80; S[n]=55
      n++; V[n]=3.85; S[n]=65
      n++; V[n]=3.90; S[n]=75
      n++; V[n]=3.95; S[n]=82
      n++; V[n]=4.00; S[n]=88
      n++; V[n]=4.05; S[n]=93
      n++; V[n]=4.10; S[n]=97
      n++; V[n]=4.15; S[n]=99
      n++; V[n]=4.20; S[n]=100

      if (v<=V[1]) { print S[1]; exit }
      if (v>=V[n]) { print S[n]; exit }

      for(i=1;i<n;i++){
        if(v>=V[i] && v<=V[i+1]){
          t=(v - V[i])/(V[i+1]-V[i])
          soc=S[i] + t*(S[i+1]-S[i])
          soc=clamp(soc,0,100)
          printf "%.0f\n", soc
          exit
        }
      }
      print 0
    }
  '
}

smooth_soc() {
  local raw="$1"
  [[ "$ENABLE_SMOOTHING" -eq 1 ]] || { echo "$raw"; return; }

  local last=""; [[ -f "$SMOOTH_FILE" ]] && last="$(cat "$SMOOTH_FILE" 2>/dev/null || true)"
  if [[ -z "$last" ]] || ! [[ "$last" =~ ^[0-9]+$ ]]; then
    echo "$raw" > "$SMOOTH_FILE"
    echo "$raw"
    return
  fi

  awk -v raw="$raw" -v last="$last" -v a="$SMOOTH_ALPHA" '
    function clamp(x,lo,hi){ return (x<lo?lo:(x>hi?hi:x)) }
    BEGIN{
      soc = (a*raw) + ((1-a)*last)
      soc = clamp(soc,0,100)
      printf "%.0f\n", soc
    }
  ' | tee "$SMOOTH_FILE" >/dev/null
  cat "$SMOOTH_FILE"
}

write_virtual_battery() {
  local soc="$1" mv="$2"

  printf "%s\n" "$soc" > "$SYS_PS/BAT0/capacity"
  printf "Battery\n" > "$SYS_PS/BAT0/type"
  printf "1\n" > "$SYS_PS/BAT0/present"

  if (( soc >= 100 )); then
    printf "Full\n" > "$SYS_PS/BAT0/status"
  else
    printf "Discharging\n" > "$SYS_PS/BAT0/status"
  fi

  printf "POWER_SUPPLY_NAME=BAT0\nPOWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_CAPACITY=%s\nPOWER_SUPPLY_VOLTAGE_NOW=%s\n" \
    "$soc" "$((mv*1000))" > "$SYS_PS/BAT0/uevent"
}

stop_virtual_battery() {
  if is_fake_active; then
    umount "$SYS_PS" || true
  fi
  if is_mounted "$REAL_SNAPSHOT"; then
    umount "$REAL_SNAPSHOT" || true
  fi
  rm -rf "$FAKE_ROOT" "$REAL_SNAPSHOT" "$SMOOTH_FILE" 2>/dev/null || true
}

status_virtual_battery() {
  echo "fake overlay: $([ is_fake_active ] && echo ACTIVE || echo INACTIVE)"
  if [[ -f "$SYS_PS/BAT0/capacity" ]]; then
    echo "BAT0 capacity: $(cat "$SYS_PS/BAT0/capacity" 2>/dev/null || true)%"
    echo "BAT0 status:   $(cat "$SYS_PS/BAT0/status" 2>/dev/null || true)"
  else
    echo "BAT0 capacity: (missing)"
  fi
}

do_update() {
  [[ ${EUID:-$(id -u)} -eq 0 ]] || die "run as root"
  command -v i2ctransfer >/dev/null 2>&1 || die "i2ctransfer not found"

  mv="$(read_word_le "$REG_VBAT_MV")" || die "failed reading voltage reg 0x$(printf "%02X" "$REG_VBAT_MV")"
  v="$(awk -v mv="$mv" 'BEGIN{printf "%.4f", mv/1000.0}')"

  soc_word="$(read_word_le "$REG_SOC" 2>/dev/null || echo 65535)"
  if (( soc_word <= 100 )); then
    raw_soc="$soc_word"
    src="reg0x$(printf "%02X" "$REG_SOC")"
  else
    raw_soc="$(soc_from_voltage_curve "$v")"
    src="curve"
  fi

  soc="$(smooth_soc "$raw_soc")"

  echo "I2C: VBAT=${v}V (${mv}mV)  SOC=${soc}% (raw=${raw_soc}% ${src})"

  ensure_virtual_battery
  write_virtual_battery "$soc" "$mv"
}

case "${1:-update}" in
  update) do_update ;;
  status) status_virtual_battery ;;
  stop)  [[ ${EUID:-$(id -u)} -eq 0 ]] || die "run as root"; stop_virtual_battery ;;
  *)
    echo "Usage:"
    echo "  $0 update   # read I2C and write /sys/class/power_supply/BAT0/*"
    echo "  $0 status"
    echo "  $0 stop"
    exit 2
    ;;
esac

