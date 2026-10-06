"""Regression tests for tools/content/build.py and the v2 texts in tools/store_sign.py.

    python tools/content/test_build.py

The fake credentials below are made up and only have the SHAPE of real ones.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import build        # noqa: E402
import store_sign   # noqa: E402

H = "a" * 64
U = "https://github.com/indecenti/nucleoos-p4-store/releases/download/content-2026.10.1/x"


def refused(fn, *a):
    try:
        fn(*a)
    except (SystemExit, ValueError):
        return True
    return False


class Secrets(unittest.TestCase):
    def test_real_shapes_are_caught(self):
        fakes = [b"AIza" + b"B" * 35, b"AQ." + b"Ab_-9" * 10, b"sk-ant-api03-" + b"x" * 60,
                 b"sk-proj-" + b"Q" * 48, b"gsk_" + b"z" * 52, b"xai-" + b"k" * 60, b"hf_" + b"m" * 34,
                 b"ghp_" + b"g" * 36, b'{"provider":"x","key":"' + b"k" * 30 + b'"}',
                 b"-----BEGIN EC PRIVATE KEY-----\n" + b"MHcCAQEEI" * 12 + b"\n", b"123456789:AA" + b"t" * 33]
        for f in fakes:
            self.assertTrue(refused(build.scan_bytes, "x", b"prefix " + f + b" suffix"), f[:10])

    def test_validators_and_placeholders_pass(self):
        ok = [b"prefix: /^(AIza|AQ\\.)/, ph: 'AIza... / AQ....'", b"if (k.startsWith('sk-ant-')) return 'anthropic'",
              b"<label>Chiave privata</label><textarea placeholder='-----BEGIN OPENSSH PRIVATE KEY-----'>",
              b'{"key": ""}', b'{"key":"short"}']
        for t in ok:
            build.scan_bytes("x", t)

    def test_error_never_echoes_the_secret(self):
        secret = b"AIza" + b"S" * 35
        try:
            build.scan_bytes("f", secret)
        except SystemExit as e:
            self.assertNotIn("SSSS", str(e))

    def test_owner_files_never_ship(self):
        for rel in ("data/anima/teacher.json", "data/anima/conv/../memory.jsonl", "nucleos/settings.nvb",
                    "data/anima/telegram.json", "web/USER.md"):
            self.assertTrue(refused(build.check_name, rel), rel)
        build.check_name("data/anima/lex-it.tsv")


class ContentText(unittest.TestCase):
    def pack(self, files, dest="data/anima"):
        return {"id": "anima-core-it", "version": "2026.10.1", "dest": dest, "files": files}

    def f(self, kind, name, size=10, url=U, url2=None):
        d = {"kind": kind, "name": name, "size": size, "sha256": H, "url": url}
        if url2:
            d["url2"] = url2
        return d

    def test_good(self):
        t = store_sign.content_pack_text(self.pack([self.f("f", "a.bin"), self.f("i", "akb5"), self.f("t", "akb5", url2=U),
                                                    self.f("f", "learned/x.jsonl")]))
        self.assertTrue(t.startswith(b"nucleoos-data-v2\nanima-core-it\n2026.10.1\ndata/anima\n"))
        self.assertIn(b" t akb5 " + U.encode() + b" " + U.encode() + b"\n", t)

    def test_rules(self):
        bad = [
            [self.f("t", "akb5")],                                        # tree without index
            [self.f("i", "akb5")],                                        # index without tree
            [self.f("i", "a"), self.f("t", "b")],
            [self.f("f", "a"), self.f("f", "b"), self.f("f", "a")],      # name again later
            [self.f("i", "."), self.f("t", "."), self.f("f", "x")],      # "." overlaps everything
            [self.f("i", "akb5"), self.f("t", "akb5"), self.f("f", "akb5/x")],
            [self.f("f", ".")], [self.f("f", "a.part")], [self.f("f", "../a")], [self.f("f", "a/b/c/d")],
            [self.f("f", "a", url="http://x/a")], [self.f("x", "a")],
            [self.f("i", "a", size=600 * 1024), self.f("t", "a")],
        ]
        for files in bad:
            self.assertTrue(refused(store_sign.content_pack_text, self.pack(files)), files)
        self.assertTrue(refused(store_sign.content_pack_text, self.pack([self.f("f", "a")], dest="anima")))
        self.assertTrue(refused(store_sign.content_pack_text, self.pack([self.f("f", "a")], dest="apps")))

    def test_tree_index(self):
        t = store_sign.tree_index_text([("b.js", 3, H), ("a/x.js", 0, H)])
        self.assertEqual(t, b"nucleoos-tree-v1\n" + H.encode() + b" 0 a/x.js\n" + H.encode() + b" 3 b.js\n")
        for bad in ([], [("a", 1, H), ("a/b", 1, H)], [("../a", 1, H)], [("a b", 1, H)], [("a", 1, "A" * 64)]):
            self.assertTrue(refused(store_sign.tree_index_text, bad), bad)

    def test_ustar_is_deterministic(self):
        files = [("a.txt", b"x"), ("q" * 60 + "/" + "r" * 60 + ".js", b"y" * 700)]   # > 100: needs the prefix field
        self.assertEqual(build.ustar(files), build.ustar(files))

    def test_every_pack_passes_the_scan(self):
        for pid, spec in build.PACKS.items():
            build.build_pack(pid, spec, "2026.10.1", "content-test", None, True)


if __name__ == "__main__":
    unittest.main()
