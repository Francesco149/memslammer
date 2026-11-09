#!/bin/sh
# Linux counterpart of events.ps1: summarise kernel-logged hardware errors since
# a UTC timestamp (ISO 8601).
#   mce:  machine-check / EDAC / memory-failure messages (WHEA's role on Windows)
#   oops: kernel oopses, kernel data-structure corruption reports and user-space
#         segfaults; random corruption from bad memory can show up here before
#         a tester notices
#   src:  where the log came from. journalctl can itself be broken by the
#         corruption we test for (a flipped bit in a shared library), so fall
#         back to the kernel ring buffer; "dmesg_wrapped" means older messages
#         were lost and the counts are a lower bound.
# Prints "EVENTS error=..." rather than zeros when no log could be read.
# An unexpected reboot isn't visible here: a live USB keeps no journal across
# boots. The harness detects it from the boot id instead.
since=$(date -u -d "$1" +%s) || exit 1
# Filter on time since boot, not wall-clock stamps: a live USB boots with the
# RTC's time (local time on a Windows box) and chrony steps the clock later,
# so early-boot messages can be stamped hours into the future.
mono=$(awk -v s="$since" -v n="$(date +%s)" '{ print s - (n - $1) }' /proc/uptime)
if log=$(journalctl -k -b --no-pager -q -o short-monotonic 2>/dev/null); then
  src=journal
  log=$(printf '%s\n' "$log" | awk -F'[][]' -v m="$mono" '$2 + 0 >= m')
else
  if ! all=$(dmesg 2>/dev/null) || [ -z "$all" ]; then
    echo "EVENTS error=no_log"
    exit 1
  fi
  log=$(printf '%s\n' "$all" | awk -F'[][]' -v m="$mono" '$2 + 0 >= m')
  first=$(printf '%s\n' "$all" | awk -F'[][]' 'NR == 1 { print $2 + 0 }')
  src=dmesg
  awk -v f="$first" -v m="$mono" 'BEGIN { exit !(f > m) }' && src=dmesg_wrapped
fi
mce_re='\[Hardware Error\]|Machine check|machine check|EDAC MC[0-9]+: [0-9]+ (CE|UE) |Memory failure'
oops_re='Oops|BUG:|general protection|segfault at|list_(add|del) corruption'
mce=$(printf '%s\n' "$log" | grep -cE "$mce_re")
oops=$(printf '%s\n' "$log" | grep -cE "$oops_re")
echo "EVENTS mce=$mce oops=$oops src=$src"
printf '%s\n' "$log" | grep -E "$mce_re|$oops_re" | head -5 | sed 's/^/EVENT /'
