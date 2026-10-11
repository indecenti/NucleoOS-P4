"""Differential test of nvzim against python-libzim (pip install libzim; GPL, used only here, never
shipped). zimdump (nvzim, built by build_native.sh) runs in WSL on copies of the files; libzim
reads the same files on Windows. Every sampled entry must agree on path, title, kind, MIME type,
redirect target, size and MD5 of the content; so must the main page and the metadata, and the
title listing must be in byte order.

    python ports/zim/tools/diff_libzim.py --wsl-dir /root/zimtest --dir D:/zimtest/fixtures \
        --dir D:/NucleoV2/tools/kb/.cache [-n 10000] FILE.zim ...
"""
import argparse
import hashlib
import os
import subprocess
import sys

import libzim.reader as R

ZIMDUMP = '/mnt/d/NucleoV2/ports/zim/_out/native/zimdump'


def wsl(*args):
    r = subprocess.run(['wsl.exe', '-d', 'Ubuntu-24.04', '--', ZIMDUMP, *args], capture_output=True)
    if r.returncode:
        raise RuntimeError(f'zimdump {args[0]} failed: {r.stderr.decode("utf-8", "replace")}')
    return r.stdout.decode('utf-8', 'replace')


def clean(s):
    return s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')


def check_file(name, win_path, wsl_path, n, seed):
    a = R.Archive(win_path)
    new = a.has_new_namespace_scheme
    bad = []

    def expect(what, ours, theirs):
        if ours != theirs:
            bad.append(f'{what}: nvzim {ours!r} != libzim {theirs!r}')

    # entries
    lines = wsl('entries', wsl_path, str(n), str(seed)).splitlines()
    checked = skipped = 0
    for line in lines:
        f = line.split('\t')
        idx = int(f[0])
        if f[1] == 'ERR':
            bad.append(f'entry {idx}: nvzim error {f[2]}')
            continue
        ns, path, title, kind, mime, size, md5 = f[1:8]
        ours = path if new else f'{ns}/{path}'
        try:
            e = a._get_entry_by_id(idx)
        except Exception as x:   # an index libzim does not expose
            skipped += 1
            continue
        expect(f'{idx} path', ours, e.path)
        expect(f'{idx} title', title, clean(e.title))
        expect(f'{idx} redirect', kind == 'redirect', e.is_redirect)
        if kind == 'redirect':
            t = e.get_redirect_entry()
            tns, tpath = mime.split('/', 1)
            expect(f'{idx} target', tpath if new else mime, clean(t.path))
        elif kind == 'content':
            it = e.get_item()
            data = bytes(it.content)
            expect(f'{idx} mime', mime, it.mimetype)
            if size == 'ERR':
                bad.append(f'{idx} read: nvzim {md5}')
            else:
                expect(f'{idx} size', int(size), len(data))
                expect(f'{idx} md5', md5, hashlib.md5(data).hexdigest())
        checked += 1

    # main page and metadata
    info = wsl('info', wsl_path)
    meta = {}
    for line in info.splitlines():
        if ': ' in line and not line.startswith(('mime:', 'version')):
            k, v = line.split(': ', 1)
            meta[k] = v
    try:
        mp = a.main_entry.get_item().path
        expect('main page', meta.get('main page'), mp if new else mp.split('/', 1)[1])
    except RuntimeError:
        pass
    for k in ('Title', 'Name', 'Language', 'Date', 'Creator', 'Publisher', 'Flavour'):
        if k in a.metadata_keys:
            expect(f'metadata {k}', meta.get(k), bytes(a.get_metadata(k)).decode('utf-8', 'replace')[:255])

    # title listing: byte order, and titles that match their entries
    rows = [r.split('\t') for r in wsl('titles', wsl_path, '2000', str(seed)).splitlines()]
    rows = sorted((int(p), int(i), t) for p, i, t in rows if i != 'ERR')
    for (p1, _, t1), (p2, _, t2) in zip(rows, rows[1:]):
        if p1 < p2 and t1.encode() > t2.encode():
            bad.append(f'listing out of order at {p1}/{p2}: {t1!r} > {t2!r}')
    for p, i, t in rows[:300]:
        expect(f'listing {p} title', t, clean(a._get_entry_by_id(i).title))

    print(f'{name}: {checked} entries checked ({skipped} skipped), {len(rows)} listing positions, '
          f'{len(bad)} differences')
    for b in bad[:20]:
        print('   ', b)
    return not bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--dir', action='append', required=True, help='Windows folder(s) with the files')
    ap.add_argument('--wsl-dir', required=True, help='the same files inside WSL')
    ap.add_argument('-n', type=int, default=10000, help='random entries per file (0 = all)')
    ap.add_argument('--seed', type=int, default=12345)
    args = ap.parse_args()
    ok = True
    for name in args.files:
        win = next((os.path.join(d, name) for d in args.dir if os.path.exists(os.path.join(d, name))), None)
        if not win:
            print(f'{name}: not found in {args.dir}')
            ok = False
            continue
        ok &= check_file(name, win, f'{args.wsl_dir}/{name}', args.n, args.seed)
    print('DIFF OK' if ok else 'DIFF FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
