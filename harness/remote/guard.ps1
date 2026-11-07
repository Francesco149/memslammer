# Prints one line describing machine state; used before every run.
# BUSY=<names> lists memory testers / heavy loads already running.
$names = 'ramtest','TM5','memslammer','karhu_drv','y-cruncher','OCCT','MemTest64','HCIMemtest','memtest'
$busy = Get-Process -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.ProcessName } |
    Select-Object -ExpandProperty ProcessName -Unique
$os = Get-CimInstance Win32_OperatingSystem
$boot = $os.LastBootUpTime.ToUniversalTime().ToString('o')
$free = [int]($os.FreePhysicalMemory / 1024)
$total = [int]($os.TotalVisibleMemorySize / 1024)
$dimms = @(Get-CimInstance Win32_PhysicalMemory)
$mts = ($dimms | Select-Object -ExpandProperty ConfiguredClockSpeed -Unique) -join '/'
"STATE boot=$boot free_mb=$free total_mb=$total mts=$mts dimms=$($dimms.Count) busy=$($busy -join ',')"
