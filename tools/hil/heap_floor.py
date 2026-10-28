#!/usr/bin/env python3
"""Hardware-in-the-loop check of the internal-RAM low-water mark under a fixed load, driven over Wi-Fi.

The static budgets in tools/ci/check_budgets.py only catch slow creep; what actually breaks the board is
internal RAM running out at run time (Wi-Fi RX pool, DMA pool, TLS). This reboots the board, lets it
settle, then for 90 s fetches web-companion pages on 4 parallel connections while the Terminal runs two
TLS jobs (update check, content refresh) and a directory listing. It fails when the internal heap's
minimum free size or its largest free block drops below the floors.

    python tools/hil/heap_floor.py [--host nucleov2.local] [--no-reboot]

Needs the paired token (%USERPROFILE%/.nucleo/token, tools/pair.py). Measured on 1.2.191 (FreeRTOS, heap
and ring-buffer code in flash): idle 320 KB free, min 280 KB, largest block 238 KB.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request

MIN_FREE = 120 * 1024       # internal heap low-water mark
MIN_LARGEST = 64 * 1024     # largest internal free block after the load (DMA / TLS buffers)
PAGES = ['/', '/apps/games/index.html', '/apps/settings/index.html', '/apps/paint/index.html',
         '/apps/store/index.html', '/apps/anima/index.html', '/store-client.js', '/webllm.js',
         '/apps/arcade/index.html', '/api/apps', '/api/status']
TERM = ['update check', 'content refresh', 'top -b -n 1', 'ls -la /sdcard/web/apps']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--host', default=os.environ.get('NV_BOARD_IP', 'nucleov2.local'))
    ap.add_argument('--no-reboot', action='store_true', help='measure from the current boot')
    a = ap.parse_args()
    base = 'http://' + a.host
    token = open(os.path.expanduser('~/.nucleo/token')).read().strip()
    nsh = [sys.executable, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'nsh.py'),
           '--host', a.host]

    def get(path, timeout=20):
        req = urllib.request.Request(base + path, headers={'Authorization': 'Bearer ' + token,
                                                           'Accept-Encoding': 'gzip'})
        with urllib.request.urlopen(req, timeout=timeout) as f:
            return f.read()

    def internal():
        return json.loads(get('/api/heap'))['internal']

    if not a.no_reboot:
        subprocess.run(nsh + ['--wait', '5', 'reboot'], capture_output=True)
        time.sleep(10)
    for _ in range(90):
        try:
            status = json.loads(get('/api/status', 4))
            break
        except Exception:
            time.sleep(3)
    else:
        sys.exit('heap_floor: the board did not come back')
    if not a.no_reboot:
        time.sleep(40)                                         # boot jobs: Wi-Fi join, auto-OTA check
    idle = internal()

    errors, stop = [], time.time() + 90

    def worker(k):
        while time.time() < stop:
            try:
                get(PAGES[k % len(PAGES)])
            except Exception as e:
                errors.append(str(e)[:80])
            k += 1

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for cmd in TERM:
        subprocess.run(nsh + ['--wait', '60', cmd], capture_output=True)
    for t in threads:
        t.join()
    after = internal()

    print('heap_floor: %s  idle free %d K  min %d K  largest %d K  (http errors %d)' % (
        status.get('version'), idle['free_bytes'] // 1024, after['min_free_bytes'] // 1024,
        after['largest_free_block'] // 1024, len(errors)))
    bad = []
    if after['min_free_bytes'] < MIN_FREE:
        bad.append('internal min free %d K < %d K' % (after['min_free_bytes'] // 1024, MIN_FREE // 1024))
    if after['largest_free_block'] < MIN_LARGEST:
        bad.append('largest internal block %d K < %d K' % (after['largest_free_block'] // 1024, MIN_LARGEST // 1024))
    if errors:
        bad.append('%d failed page loads (first: %s)' % (len(errors), errors[0]))
    if bad:
        print('heap_floor: FAIL - ' + '; '.join(bad))
        return 1
    print('heap_floor: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
