# usage:  .\build.ps1          compile only
#         .\build.ps1 flash    compile, flash the watch (port found automatically), then set its clock from this PC
. C:\esp\esp-idf-v5.5.5\export.ps1 *> $null
function Find-WatchPort {
    $d = Get-PnpDevice -PresentOnly -Class Ports | Where-Object { $_.InstanceId -match 'VID_303A' } | Select-Object -First 1
    if ($d -and $d.FriendlyName -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}
if ($args[0] -eq 'flash') {
    $port = Find-WatchPort
    if (-not $port) { "Watch not found on USB (Espressif VID 303A). Plug it in and try again."; exit 1 }
    "Watch on $port"
    $out = idf.py -DSNAP=0 -DSELFTEST=0 build 2>&1
    if ($LASTEXITCODE -ne 0) { $out | Select-String -Pattern 'error:|undefined reference|FAILED' | Select -First 10; "BUILD FAILED - nothing was flashed"; exit 1 }
    idf.py -p $port flash 2>&1 | Select-String 'error|Error|Hard resetting'
    Start-Sleep 5
    python tools\settime.py $port
} else { idf.py -DSNAP=0 -DSELFTEST=0 build 2>&1 | Select-String -Pattern 'error:|warning:|undefined reference|FAILED|binary size|Project build' | Select -First 40 }
