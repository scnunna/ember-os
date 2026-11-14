# Build + flash the self-test firmware and print the TEST lines.   usage: .\selftest.ps1
. C:\esp\esp-idf-v5.5.5\export.ps1 *> $null
idf.py -DSELFTEST=1 build 2>&1 | Select-String -Pattern 'error:|undefined reference|FAILED' | Select -First 20
idf.py -p COM9 flash 2>&1 | Select-String 'rror'
Start-Sleep 1
python -c "
import serial,time
s=serial.Serial('COM9',115200,timeout=1); s.dtr=False; s.rts=True; time.sleep(.2); s.rts=False
t=time.time()
while time.time()-t<60:
    l=s.readline().decode(errors='replace').rstrip()
    if l and any(k in l for k in ('TEST','E (','rec:','Guru','abort','assert','panic')): print(l)
    if 'SELFTEST COMPLETE' in l: break
"
