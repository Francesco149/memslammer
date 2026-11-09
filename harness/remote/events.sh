#!/bin/sh
# Linux counterpart of events.ps1: summarise kernel-logged hardware errors since
# a UTC timestamp (ISO 8601).
#   mce:  machine-check / EDAC / memory-failure messages (WHEA's role on Windows)
#   oops: kernel oopses and user-space segfaults; random corruption from bad
#         memory can show up here before a tester notices
# An unexpected reboot isn't visible here: a live USB keeps no journal across
# boots. The harness detects it from the boot id instead.
since=$(date -u -d "$1" +%s) || exit 1
log=$(journalctl -k --no-pager -q -o short-iso --since "@$since" 2>/dev/null)
mce_re='\[Hardware Error\]|Machine check|machine check|EDAC MC[0-9]+: [0-9]+ (CE|UE) |Memory failure'
oops_re='Oops|BUG:|general protection|segfault at'
mce=$(printf '%s\n' "$log" | grep -cE "$mce_re")
oops=$(printf '%s\n' "$log" | grep -cE "$oops_re")
echo "EVENTS mce=$mce oops=$oops"
printf '%s\n' "$log" | grep -E "$mce_re|$oops_re" | head -5 | sed 's/^/EVENT /'
