#!/usr/bin/env python3
"""Regenerate the synthetic ZIM files of the nvzim tests (ports/zim/nvzim): the fuzz seeds in
corpus/nvzim and the expectations in unit/nvzim_fixtures.h. Committed, so this only runs when the
fixtures change. Our own content, no third-party data. Needs the `zstandard` module.

    python tests/host/gen_zim.py

new.zim  ZIM 6.3, namespaces C M W X: articles in zstd clusters (one decodes to more than 2 MiB),
         redirects (a chain, a loop), accented and lowercase titles, images in a raw cluster, an
         empty blob, clusters with 8-byte blob offsets (raw and zstd), metadata, W/mainPage and the
         X/listing/titleOrdered/v1 title listing in a raw cluster, like the files of openZIM.
old.zim  ZIM 5.0, namespaces A I - M: xz clusters, a title index in the header, a redirect.
"""
import hashlib
import lzma
import os
import struct

import zstandard

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "corpus", "nvzim")
HEADER = os.path.join(HERE, "unit", "nvzim_fixtures.h")
REDIRECT = 0xFFFF


class Entry:
    def __init__(self, ns, path, title="", mime=None, content=None, target=None, cluster=None):
        self.ns, self.path, self.title, self.mime = ns, path, title, mime
        self.content, self.target, self.cluster = content, target, cluster   # target = (ns, path)
        self.index = None


def article(i, size=0):
    body = f"<p>Article {i:03d}: " + "lorem ipsum dolor sit amet " * 3 + "</p>"
    html = f"<html><head><title>Article {i:03d}</title></head><body><h1>Article {i:03d}</h1>{body}"
    if size:
        html += ("<p>" + f"filler {i} " * 40 + "</p>\n") * (size // 400)
    return (html + "</body></html>").encode()


def blob_cluster(blobs, ext):
    w = 8 if ext else 4
    offs, pos = [], w * (len(blobs) + 1)
    for b in blobs:
        offs.append(pos)
        pos += len(b)
    offs.append(pos)
    fmt = "<Q" if ext else "<I"
    return b"".join(struct.pack(fmt, o) for o in offs) + b"".join(blobs)


def build(entries, clusters, major, minor, mimes, main=None, title_index=False, listing=None):
    """clusters: {name: (kind, ext)} with kind 'zstd' 'xz' 'raw'. Returns the file bytes."""
    entries.sort(key=lambda e: (e.ns.encode(), e.path.encode()))
    by_key = {}
    for i, e in enumerate(entries):
        e.index = i
        by_key[(e.ns, e.path)] = e
    if listing:   # X/listing/titleOrdered/v1: front articles by title (redirects included, as openZIM)
        front = sorted((e for e in entries if e.ns == "C" and e.mime in (None, "text/html")),
                       key=lambda e: (e.title or e.path).encode())
        listing.content = b"".join(struct.pack("<I", e.index) for e in front)
    names = list(clusters)
    members = {n: [e for e in entries if e.cluster == n] for n in names}
    for n in names:
        for b, e in enumerate(members[n]):
            e.blob_no = b
    mime_list = b"".join(m.encode() + b"\0" for m in mimes) + b"\0"
    out = bytearray(80) + mime_list
    cluster_pos = []
    for n in names:
        kind, ext = clusters[n]
        data = blob_cluster([e.content for e in members[n]], ext)
        info = {"raw": 1, "xz": 4, "zstd": 5}[kind] | (0x10 if ext else 0)
        if kind == "zstd":
            data = zstandard.ZstdCompressor(level=19, write_content_size=False).compress(data)
        elif kind == "xz":
            data = lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32)
        cluster_pos.append(len(out))
        out += bytes([info]) + data
    dirent_pos = []
    for e in entries:
        dirent_pos.append(len(out))
        title = e.title.encode() if e.title and e.title != e.path else b""
        if e.target:
            head = struct.pack("<HBcII", REDIRECT, 0, e.ns.encode(), 0, by_key[e.target].index)
        else:
            head = struct.pack("<HBcIII", mimes.index(e.mime), 0, e.ns.encode(), 0,
                               names.index(e.cluster), e.blob_no)
        out += head + e.path.encode() + b"\0" + title + b"\0"
    path_ptr = len(out)
    out += b"".join(struct.pack("<Q", p) for p in dirent_pos)
    cluster_ptr = len(out)
    out += b"".join(struct.pack("<Q", p) for p in cluster_pos)
    title_pos = 0xFFFFFFFFFFFFFFFF
    if title_index:   # ZIM 5: every entry, by namespace then title
        title_pos = len(out)
        order = sorted(entries, key=lambda e: (e.ns.encode(), (e.title or e.path).encode()))
        out += b"".join(struct.pack("<I", e.index) for e in order)
    checksum = len(out)
    main_index = by_key[main].index if main else 0xFFFFFFFF
    struct.pack_into("<IHH16sIIQQQQIIQ", out, 0, 0x044D495A, major, minor, b"nvzim-test-uuid!",
                     len(entries), len(names), path_ptr, title_pos, cluster_ptr, 80, main_index,
                     0xFFFFFFFF, checksum)
    out += hashlib.md5(out).digest()
    return bytes(out)


def new_zim():
    mimes = ["application/octet-stream+zimlisting", "image/png", "text/html", "text/plain"]
    E = []
    for i in range(150):   # 5 small zstd clusters
        E.append(Entry("C", f"Article_{i:03d}", f"Article {i:03d}", "text/html", article(i), cluster=f"z{i // 30}"))
    for i in range(150, 200):   # one cluster that decodes to more than 2 MiB
        E.append(Entry("C", f"Article_{i:03d}", f"Article {i:03d}", "text/html", article(i, 64 * 1024), cluster="zbig"))
    extra = [("Main_Page", "Main Page"), ("Diabete_mellito", "Diabete mellito"), ("Città_di_prova", "Città di prova"),
             ("città_minuscola", "città minuscola"), ("Élan", "Élan"), ("iPhone", "iPhone"),
             ("Zebra", "")]
    for k, (p, t) in enumerate(extra):
        E.append(Entry("C", p, t, "text/html", f"<html><body><h1>{t or p}</h1>{k}</body></html>".encode(), cluster="zx"))
    E += [Entry("C", "Diabete_Mellito", "Diabete Mellito", target=("C", "Diabete_mellito")),
          Entry("C", "Diabete", "Diabete", target=("C", "Diabete_mellito")),
          Entry("C", "R1", "R1", target=("C", "R2")), Entry("C", "R2", "R2", target=("C", "R3")),
          Entry("C", "R3", "R3", target=("C", "Article_001")),
          Entry("C", "LoopA", "LoopA", target=("C", "LoopB")), Entry("C", "LoopB", "LoopB", target=("C", "LoopA"))]
    for i in range(12):
        png = b"\x89PNG\r\n\x1a\n" + bytes((i * 7 + j) & 0xFF for j in range(300 + 50 * i))
        E.append(Entry("C", f"img/pic_{i:02d}.png", f"img/pic_{i:02d}.png", "image/png", png, cluster="raw"))
    E.append(Entry("C", "img/empty.bin", "", "text/plain", b"", cluster="raw"))
    for i in range(4):   # 8-byte blob offsets
        E.append(Entry("C", f"wide/raw_{i}.bin", "", "text/plain", bytes([i]) * (1000 + i), cluster="rawext"))
        E.append(Entry("C", f"wide/zst_{i}.txt", "", "text/plain", (f"wide {i} " * 500).encode(), cluster="zext"))
    E += [Entry("M", "Title", "", "text/plain", b"nvzim test file", cluster="meta"),
          Entry("M", "Language", "", "text/plain", b"ita", cluster="meta"),
          Entry("M", "Name", "", "text/plain", b"nvzim_test", cluster="meta"),
          Entry("M", "Date", "", "text/plain", b"2026-10-11", cluster="meta"),
          Entry("M", "Illustration_48x48@1", "", "image/png", b"\x89PNG\r\n\x1a\nfake-icon", cluster="meta"),
          Entry("W", "mainPage", "", target=("C", "Main_Page"))]
    listing = Entry("X", "listing/titleOrdered/v1", "", "application/octet-stream+zimlisting", b"", cluster="lst")
    E.append(listing)
    clusters = {f"z{i}": ("zstd", False) for i in range(5)}
    clusters.update({"zbig": ("zstd", False), "zx": ("zstd", False), "raw": ("raw", False),
                     "rawext": ("raw", True), "zext": ("zstd", True), "meta": ("zstd", False), "lst": ("raw", False)})
    return build(E, clusters, 6, 3, mimes, main=("W", "mainPage"), listing=listing), E


def old_zim():
    mimes = ["text/html", "image/png", "text/css", "text/plain"]
    E = [Entry("A", f"Page_{i:02d}.html", f"Page {i:02d}", "text/html", article(i), cluster=f"x{i // 10}") for i in range(40)]
    E += [Entry("A", "Accueil.html", "Accueil", "text/html", b"<html><body>Accueil</body></html>", cluster="x0"),
          Entry("I", "favicon.png", "", "image/png", b"\x89PNG\r\n\x1a\nold-icon", cluster="img"),
          Entry("-", "favicon", "", target=("I", "favicon.png")),
          Entry("-", "style.css", "", "text/css", b"body{margin:0}", cluster="img"),
          Entry("M", "Title", "", "text/plain", b"Old test", cluster="img")]
    clusters = {f"x{i}": ("xz", False) for i in range(4)}
    clusters["img"] = ("raw", False)
    return build(E, clusters, 5, 0, mimes, main=("A", "Accueil.html"), title_index=True), E


def c_str(s):
    out = '"'
    for b in s.encode():
        out += chr(b) if 32 <= b < 127 and chr(b) not in '"\\?' else f"\\{b:03o}"
    return out + '"'


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    lines = ["// Generated by tests/host/gen_zim.py: what every entry of corpus/nvzim/*.zim holds.",
             "#pragma once", "#include <cstdint>", "",
             "struct NvzimFx { const char *file; uint32_t index; char ns; const char *path, *title;",
             "                 bool redirect; const char *target; uint32_t size; const char *md5; };", "",
             "static const NvzimFx kNvzimFx[] = {"]
    counts = []
    for name, (data, entries) in (("new.zim", new_zim()), ("old.zim", old_zim())):
        with open(os.path.join(OUT_DIR, name), "wb") as f:
            f.write(data)
        print(f"corpus/nvzim/{name:8s} {len(data):7d} B, {len(entries)} entries")
        counts.append((name, len(entries)))
        for e in entries:
            title = e.title or e.path
            if e.target:
                tgt = next(x for x in entries if (x.ns, x.path) == e.target)
                lines.append(f'    {{"{name}", {e.index}, \'{e.ns}\', {c_str(e.path)}, {c_str(title)}, true, '
                             f'{c_str(tgt.ns + "/" + tgt.path)}, 0, ""}},')
            else:
                lines.append(f'    {{"{name}", {e.index}, \'{e.ns}\', {c_str(e.path)}, {c_str(title)}, false, "", '
                             f'{len(e.content)}, "{hashlib.md5(e.content).hexdigest()}"}},')
    lines += ["};", ""]
    for name, n in counts:
        lines.append(f"static const uint32_t kNvzim{name.split('.')[0].capitalize()}Entries = {n};")
    with open(HEADER, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"unit/nvzim_fixtures.h  {len(lines)} lines")


if __name__ == "__main__":
    main()
