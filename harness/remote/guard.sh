#!/bin/sh
# Linux counterpart of guard.ps1: prints one line describing machine state;
# used before every run. busy= lists memory testers / heavy loads already running.
busy=$(pgrep -l -x 'memslammer|memtester|stressapptest|stress-ng|y-cruncher' \
       | awk '{print $2}' | sort -u | paste -sd, -)
boot=$(cat /proc/sys/kernel/random/boot_id)
# MemAvailable, not MemFree: page cache is reclaimable and often several GB.
free=$(awk '/^MemAvailable:/ {print int($2 / 1024)}' /proc/meminfo)
total=$(awk '/^MemTotal:/ {print int($2 / 1024)}' /proc/meminfo)
mts=$(dmidecode -t 17 2>/dev/null | awk -F': ' '/Configured Memory Speed: [0-9]/ {print $2 + 0}' | sort -u | paste -sd/ -)
dimms=$(dmidecode -t 17 2>/dev/null | grep -cE '^\s+Size: [0-9]')
echo "STATE boot=$boot free_mb=$free total_mb=$total mts=$mts dimms=$dimms busy=$busy"
