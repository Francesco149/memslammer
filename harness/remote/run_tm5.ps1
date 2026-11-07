# Headless TestMem5 run: writes TM5.ini, launches TM5, follows Log.txt.
# Prints LOG lines as they appear and a final RESULT line.
param(
    [Parameter(Mandatory)][string]$Dir,        # folder with TM5.exe, TM5.dll, bin\
    [Parameter(Mandatory)][string]$Config,     # e.g. "Absolut @ anta777.cfg"
    [Parameter(Mandatory)][double]$Seconds
)
$ErrorActionPreference = 'Stop'
Set-Location $Dir
Get-Process TM5 -ErrorAction SilentlyContinue | Stop-Process -Force
$minutes = [math]::Max(1, [math]::Ceiling($Seconds / 60))
@(
    '[Settings]'
    "Config File=$Config"
    "Minutes=$minutes"
    'Stop On Error=1'
    'Enable Sound=0'
    'Enable Notifications=0'
    'Show Log=1'
) | Set-Content -Encoding ascii TM5.ini
Remove-Item Log.txt, Crash.log -ErrorAction SilentlyContinue

$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process .\TM5.exe -PassThru
"START tester=tm5 config=$Config"
$seen = 0; $first = -1.0; $errors = 0; $outcome = 'timeout'
while ($true) {
    Start-Sleep -Milliseconds 250
    $t = $sw.Elapsed.TotalSeconds
    if (Test-Path Log.txt) {
        $lines = @(Get-Content Log.txt -Encoding UTF8)
        for ($i = $seen; $i -lt $lines.Count; $i++) {
            $l = $lines[$i]
            "LOG t=$('{0:F1}' -f $t) $l"
            if ($l -match 'Error in test') { $errors++; if ($first -lt 0) { $first = $t } }
            elseif ($l -match 'Testing completed') { $outcome = 'completed' }
            elseif ($l -match 'Testing stopped') { if ($outcome -eq 'timeout') { $outcome = 'stopped' } }
            elseif ($l -match 'Critical error|Failed to allocate|Failed to load') { $outcome = 'tm5_failure' }
        }
        $seen = $lines.Count
    }
    if ($errors -gt 0) { break }
    if ($outcome -ne 'timeout') { break }
    if ($t -ge $Seconds) { break }
    if ((Get-Process -Id $p.Id -ErrorAction SilentlyContinue) -eq $null) { $outcome = 'exited'; break }
}
Get-Process TM5 -ErrorAction SilentlyContinue | Stop-Process -Force
$crash = Test-Path Crash.log
"RESULT tester=tm5 errors=$errors first_error_s=$('{0:F3}' -f $first) elapsed_s=$('{0:F3}' -f $sw.Elapsed.TotalSeconds) outcome=$outcome crashlog=$crash"
