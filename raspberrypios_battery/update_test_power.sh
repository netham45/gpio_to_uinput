#!/bin/bash
set -euo pipefail

## This script assumes test_power is available and uses it to create a more proper virtual battery

BUS=1
ADDR=0x42
TP=/sys/module/test_power/parameters

# Fallback SOC mapping if 0x0D isn't implemented by your Arduino
V_EMPTY=3.00
V_FULL=4.20

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

main() {
  [[ ${EUID:-$(id -u)} -eq 0 ]] || die "run as root"

  [[ -d "$TP" ]] || modprobe test_power 2>/dev/null || true
  [[ -d "$TP" ]] || die "test_power not present"

  mv="$(read_word_le 0x09)" || die "failed reading voltage reg 0x09"
  v="$(awk -v mv="$mv" 'BEGIN{printf "%.4f", mv/1000.0}')"

  soc_word="$(read_word_le 0x0D 2>/dev/null || echo 65535)"
  if (( soc_word <= 100 )); then
    soc="$soc_word"
    src="reg0x0D"
  else
    soc="$(awk -v v="$v" -v ve="$V_EMPTY" -v vf="$V_FULL" 'BEGIN{
      p=(v-ve)*100/(vf-ve);
      if(p<0)p=0; if(p>100)p=100;
      printf "%.0f", p
    }')"
    src="computed"
  fi

  echo "VBAT=${v}V (${mv}mV)  SOC=${soc}% (${src})"

  echo "$soc" > "$TP/battery_capacity"
  echo "$mv"  > "$TP/battery_voltage"
}

main "$@"
