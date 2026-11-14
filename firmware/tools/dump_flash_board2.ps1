param([string]$Port = "COM3")
$out = Join-Path $PSScriptRoot "..\..\Backup_Factory_Board2_2026-11-13"
. C:\esp\esp-idf-v5.5.5\export.ps1 *> $null
$chunk = 0x400000
for ($i = 0; $i -lt 8; $i++) {
    $addr = $i * $chunk
    $f = "$out\part$i.bin"
    if ((Test-Path $f) -and (Get-Item $f).Length -eq $chunk) { "chunk $i already done"; continue }
    for ($try = 1; $try -le 3; $try++) {
        "chunk $i (0x{0:X}) try $try  $(Get-Date -Format HH:mm:ss)" -f $addr
        python -m esptool --chip esp32s3 -p $Port read_flash $addr $chunk $f 2>&1 | Select-String 'Read|error|Error|Fatal' | ForEach-Object { "  $_" }
        if ((Test-Path $f) -and (Get-Item $f).Length -eq $chunk) { break }
    }
    if (-not ((Test-Path $f) -and (Get-Item $f).Length -eq $chunk)) { "FAILED chunk $i"; exit 1 }
}
$full = "$out\full_flash_32MB_factory_board2.bin"
$fs = [System.IO.File]::Create($full)
for ($i = 0; $i -lt 8; $i++) { $b = [System.IO.File]::ReadAllBytes("$out\part$i.bin"); $fs.Write($b, 0, $b.Length) }
$fs.Close()
Remove-Item "$out\part*.bin"
$h = (Get-FileHash $full -Algorithm SHA256).Hash
"SHA256 $h  full_flash_32MB_factory_board2.bin  stock ESP-Brookesia firmware (2026-10-15 build), dumped $(Get-Date -Format 'yyyy-MM-dd HH:mm')" | Out-File "$out\SHA256.txt" -Encoding utf8
"DONE $h"
