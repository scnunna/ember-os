# Build + flash a screenshot build, dump every screen over USB, convert to PNGs in .\snaps\   usage: .\snap.ps1
. C:\esp\esp-idf-v5.5.5\export.ps1 *> $null
idf.py -DSNAP=1 -DSELFTEST=0 build 2>&1 | Select-String -Pattern 'error:|undefined reference' | ForEach-Object { $_.ToString().Substring(0,[Math]::Min(220,$_.ToString().Length)) } | Select -First 10
idf.py -p COM9 flash 2>&1 | Select-String 'rror'
Start-Sleep 1
python snap_capture.py
