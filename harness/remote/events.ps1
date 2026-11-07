# Summarise hardware-error events logged since a UTC timestamp.
# WHEA-Logger: corrected/uncorrected hardware errors (on Ryzen, ID 19 is
# typically a corrected fabric/bus error, a sign of marginal FCLK/IMC settings).
# Kernel-Power 41 / BugCheck 1001: unexpected reboot / BSOD.
param([Parameter(Mandatory)][string]$SinceUtc)
$since = [datetime]::Parse($SinceUtc).ToLocalTime()
$ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since } -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -eq 'Microsoft-Windows-WHEA-Logger' -or
                   ($_.ProviderName -eq 'Microsoft-Windows-Kernel-Power' -and $_.Id -eq 41) -or
                   ($_.ProviderName -eq 'Microsoft-Windows-WER-SystemErrorReporting' -and $_.Id -eq 1001) })
$whea = @($ev | Where-Object ProviderName -eq 'Microsoft-Windows-WHEA-Logger')
$ids = ($whea | Group-Object Id | ForEach-Object { "$($_.Name)x$($_.Count)" }) -join ','
$kp = @($ev | Where-Object { $_.Id -eq 41 }).Count
$bc = @($ev | Where-Object { $_.Id -eq 1001 }).Count
"EVENTS whea=$($whea.Count) whea_ids=$ids kernel_power41=$kp bugcheck=$bc"
foreach ($e in ($ev | Select-Object -First 5)) {
    $msg = ($e.Message -split "`n")[0].Trim()
    "EVENT $($e.TimeCreated.ToUniversalTime().ToString('o')) $($e.ProviderName) $($e.Id) $msg"
}
