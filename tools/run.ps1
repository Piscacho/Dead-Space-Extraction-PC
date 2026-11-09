# Runs DSE.exe for a few seconds (default 12), stops it, and prints the log with the
# watchdog lines annotated with SDK names from config/sdk_names.txt.
param([int]$Seconds = 12)
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "project\build\DSE.exe"
$log = Join-Path $root "logs\run.log"
$p = Start-Process -FilePath $exe -ArgumentList "--jugar" -RedirectStandardError $log -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process -Id $p.Id -Force; "stopped after $Seconds s" } else { "exit code $($p.ExitCode)" }

$names = @{}
Get-Content (Join-Path $root "config\sdk_names.txt") | ForEach-Object {
    $parts = $_ -split "\s+"
    $names[[Convert]::ToUInt32($parts[0].Substring(2), 16)] = $parts[1]
}
$keys = $names.Keys | Sort-Object
function NameOf([uint32]$a) {
    # Nearest named function at or below the address.
    $best = $null
    foreach ($k in $keys) { if ($k -le $a) { $best = $k } else { break } }
    if ($null -eq $best) { return "" }
    if ($best -eq $a) { return $names[$best] }
    return "{0}+0x{1:X}?" -f $names[$best], ($a - $best)
}
Get-Content $log | ForEach-Object {
    if ($_ -match "guest function 0x([0-9A-F]{8})") {
        $_ + "  [" + (NameOf ([Convert]::ToUInt32($Matches[1], 16))) + "]"
    } else { $_ }
}
