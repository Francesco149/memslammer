#!/bin/sh
# Log temperatures and CPU clock once a second until killed, for lining up
# errors with DRAM temperature:
#   sampler.sh out.tsv &    ...    pkill -f '[s]ampler\.sh'
# Columns: box epoch, Tctl, Tccd1 (k10temp), one column per DDR5 DIMM sensor
# (spd5118, named by its SMBus address; DDR4 has none), all in milli-degC,
# then the mean scaling_cur_freq over all CPUs in kHz.
out=$1
K= S= H="t tctl tccd"
for h in /sys/class/hwmon/hwmon*; do
  case $(cat "$h/name") in
    k10temp) K=$h ;;
    spd5118) S="$S $h"; H="$H dimm$(readlink -f "$h/device" | sed 's/.*-00//')" ;;
  esac
done
echo "$H khz" > "$out"
while :; do
  line="$(date +%s.%N | cut -c1-14) $(cat $K/temp1_input) $(cat $K/temp3_input 2>/dev/null || echo 0)"
  for s in $S; do line="$line $(cat $s/temp1_input)"; done
  f=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq | awk '{s+=$1} END {print int(s/NR)}')
  echo "$line $f" >> "$out"
  sleep 1
done
