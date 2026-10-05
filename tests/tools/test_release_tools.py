"""Regression tests for the release tooling that guards the fleet (no board, no keys needed):

  * tools/check_sdkconfig.py - a stale sdkconfig or an eFuse-burning option stops the build
  * tools/ota_sign.py        - an image without the production key is never signed (lock-out guard),
                               the signed message format the firmware verifies
  * tools/dist.py            - rollout / channel validation

  python -m unittest discover -s tests/tools -v
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write(dirpath, name, text):
    p = os.path.join(dirpath, name)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return p


class CheckSdkconfig(unittest.TestCase):
    def run_check(self, sdkconfig, defaults, env_extra=None):
        with tempfile.TemporaryDirectory() as d:
            s = write(d, "sdkconfig", sdkconfig)
            ds = [write(d, "defaults%d" % i, t) for i, t in enumerate(defaults)]
            env = dict(os.environ)
            env.pop("NV_ALLOW_EFUSE_SECURITY", None)
            env.update(env_extra or {})
            r = subprocess.run([sys.executable, os.path.join(TOOLS, "check_sdkconfig.py"), s] + ds,
                               capture_output=True, text=True, env=env)
            return r.returncode, r.stdout

    def test_matching_config_passes(self):
        rc, _ = self.run_check("CONFIG_A=y\nCONFIG_B=320\n# CONFIG_C is not set\n",
                               ["CONFIG_A=y\nCONFIG_B=320\nCONFIG_C=n\n"])
        self.assertEqual(rc, 0)

    def test_stale_value_fails(self):
        rc, out = self.run_check("CONFIG_LV_MEM_SIZE_KILOBYTES=192\n", ["CONFIG_LV_MEM_SIZE_KILOBYTES=320\n"])
        self.assertEqual(rc, 1)
        self.assertIn("CONFIG_LV_MEM_SIZE_KILOBYTES", out)

    def test_bool_off_in_sdkconfig_on_in_defaults(self):
        rc, out = self.run_check("# CONFIG_BT_NIMBLE_ROLE_PERIPHERAL is not set\n",
                                 ["CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y\n"])
        self.assertEqual(rc, 1)
        self.assertIn("ROLE_PERIPHERAL", out)

    def test_later_defaults_file_wins(self):
        rc, _ = self.run_check("CONFIG_X=8\n", ["CONFIG_X=4\n", "CONFIG_X=8\n"])
        self.assertEqual(rc, 0)

    def test_unknown_symbol_is_ignored(self):
        rc, _ = self.run_check("CONFIG_A=y\n", ["CONFIG_A=y\nCONFIG_GONE=y\n"])
        self.assertEqual(rc, 0)

    def test_nvs_encryption_refused(self):
        rc, out = self.run_check("CONFIG_NVS_ENCRYPTION=y\n", ["CONFIG_A=y\n"])
        self.assertEqual(rc, 1)
        self.assertIn("eFuse", out)

    def test_nvs_encryption_with_opt_in(self):
        rc, _ = self.run_check("CONFIG_NVS_ENCRYPTION=y\n", ["# CONFIG_NVS_ENCRYPTION is not set\n"],
                               {"NV_ALLOW_EFUSE_SECURITY": "1"})
        self.assertEqual(rc, 0)

    def test_repo_defaults_pin_nvs_encryption_off(self):
        with open(os.path.join(ROOT, "sdkconfig.defaults"), encoding="utf-8") as f:
            self.assertIn("# CONFIG_NVS_ENCRYPTION is not set", f.read())


class OtaSign(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.m = load("ota_sign")
        except ImportError as e:   # cryptography missing on this machine
            raise unittest.SkipTest(str(e))

    def test_message_format_matches_firmware(self):
        # nv_ota_manifest::message() - a drift makes every signed update fail on the device
        self.assertEqual(self.m.message("1.2.3", "ab" * 32, 42),
                         ("nucleoos-ota-v1\n1.2.3\n" + "ab" * 32 + "\n42\n").encode())

    def test_image_without_release_key_is_refused(self):
        fake = b"\xe9" + b"\0" * 4096   # no PEM inside: a test-key build, or a key change gone wrong
        self.assertFalse(self.m.embeds_release_key(fake))
        with self.assertRaises(SystemExit) as cm:
            self.m.sign_fields("9.9.9", fake)   # refused before the private key is even loaded
        self.assertIn("refusing to sign", str(cm.exception))

    def test_image_with_release_key_is_recognised(self):
        pem = self.m.pem_bytes(self.m.PUB_PATH).strip()
        self.assertTrue(self.m.embeds_release_key(b"\0" * 100 + pem + b"\0" * 100))

    def test_backup_key_is_present(self):
        self.assertTrue(os.path.isfile(self.m.PUB_BACKUP_PATH), "the firmware must embed a backup key")
        self.assertNotEqual(self.m.pem_bytes(self.m.PUB_PATH), self.m.pem_bytes(self.m.PUB_BACKUP_PATH))

    def test_bad_manifest_does_not_verify(self):
        self.assertEqual(self.m.verify({"version": "1", "sha256": "00" * 32, "size": 1, "sig": "00" * 70}),
                         "bad signature")
        self.assertEqual(self.m.verify({"version": "1"}), "bad signature")


class Dist(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.m = load("dist")
        except ImportError as e:
            raise unittest.SkipTest(str(e))

    def test_channel_dirs(self):
        self.assertEqual(self.m.channel_dir(True, "stable"), "ota/v2")
        self.assertEqual(self.m.channel_dir(True, "beta"), "ota/v2/beta")
        self.assertEqual(self.m.channel_dir(False, "stable"), "ota")
        with self.assertRaises(SystemExit):
            self.m.channel_dir(False, "beta")

    def test_rollout_range(self):
        for ok in (0, 10, 100):
            self.assertEqual(self.m.check_rollout(ok), ok)
        for bad in (-1, 101):
            with self.assertRaises(SystemExit):
                self.m.check_rollout(bad)


if __name__ == "__main__":
    unittest.main()
