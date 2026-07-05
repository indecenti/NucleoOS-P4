# Reset the board over COM5 and print a COMPACT OTA verdict (not the whole boot log) so a
# verification costs few tokens. Run via run_verify.ps1 (needs the IDF python env's pyserial).
import serial, time, re

PORT, BAUD, MAX = 'COM5', 115200, 210

p = serial.Serial(PORT, BAUD, timeout=0.2)
p.setDTR(False); p.setRTS(True); time.sleep(0.15); p.setRTS(False)   # hard reset, run mode
t0, buf = time.time(), ''
while time.time() - t0 < MAX:
    d = p.read(4096)
    if not d:
        continue
    buf += d.decode('utf-8', 'replace')
    # installed then rebooted into the new image (>=2 version banners) and confirmed valid -> done
    if 'installed OK' in buf and buf.count('App version') >= 2 and ('marked valid' in buf or 'up-to-date' in buf):
        break
    # nothing to install (already latest) -> stop once we've seen the check result
    if 'up-to-date' in buf and 'INSTALL' not in buf and time.time() - t0 > 15:
        break
p.close()

vers    = [l.split('App version:')[1].strip() for l in buf.splitlines() if 'App version:' in l]
offered = [l.strip() for l in buf.splitlines() if 'offered v' in l]
installed  = 'installed OK' in buf
markvalid  = 'marked valid' in buf
uptodate   = 'up-to-date' in buf
panics     = buf.count('Guru Meditation')

print('=== OTA verify ===')
print('versions booted :', ' -> '.join(vers) if vers else '(none captured)')
for l in offered[-2:]:
    print('               :', l.split('ota:')[-1].strip())
print(f'installed={installed}  marked_valid={markvalid}  up_to_date={uptodate}  panics={panics}')
ok = (panics == 0) and (installed or uptodate)
print('RESULT:', 'PASS' if ok else 'CHECK - not updated this boot (retry; Wi-Fi may not have linked)')
