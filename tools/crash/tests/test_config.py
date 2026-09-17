# admin.env, the private key file and `crash keygen`.
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest

sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.dirname(os.path.dirname(os.path.abspath(__file__)))]

import support  # noqa: E402
from d2vcrash import config, seal  # noqa: E402

CRASH_PY = os.path.join(support.CRASH_DIR, "crash.py")


def write(path, text, mode=0o600):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    os.chmod(path, mode)


class ConfigFileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.addCleanup(self.tmp.cleanup)
        self.env_path = os.path.join(self.dir, "admin.env")

    def test_reads_every_key_and_defaults_the_issue_repository(self):
        write(self.env_path, "# admin settings\nAPI_BASE=https://example.invalid\n"
                             "ADMIN_TOKEN = secret-token \nGITHUB_TOKEN=\"ghp_x\"\n\n")
        cfg = config.load(self.dir)
        self.assertEqual(cfg.api_base, "https://example.invalid")
        self.assertEqual(cfg.admin_token, "secret-token")
        self.assertEqual(cfg.github_token, "ghp_x")
        self.assertEqual(cfg.issues_repo, "Franckrst/D2Vita")
        self.assertEqual(cfg.config_dir, self.dir)

    def test_a_missing_file_is_not_an_error_until_a_value_is_needed(self):
        cfg = config.load(self.dir)
        self.assertIsNone(cfg.api_base)
        with self.assertRaises(config.ConfigError) as caught:
            cfg.require_api()
        self.assertIn("admin.env", str(caught.exception))

    def test_refuses_a_file_readable_by_group_or_others(self):
        write(self.env_path, "API_BASE=https://example.invalid\n", mode=0o640)
        with self.assertRaises(config.ConfigError) as caught:
            config.load(self.dir)
        self.assertIn("chmod 600", str(caught.exception))

    def test_refuses_a_malformed_line_and_a_bad_issue_repository(self):
        write(self.env_path, "API_BASE https://example.invalid\n")
        with self.assertRaises(config.ConfigError):
            config.load(self.dir)
        write(self.env_path, "ISSUES_REPO=not a repo\n")
        with self.assertRaises(config.ConfigError):
            config.load(self.dir)

    def test_never_shows_a_token(self):
        write(self.env_path, "API_BASE=https://example.invalid\nADMIN_TOKEN=super-secret\nGITHUB_TOKEN=ghp_secret\n")
        cfg = config.load(self.dir)
        for text in (repr(cfg), str(cfg), json.dumps(cfg.describe())):
            self.assertNotIn("super-secret", text)
            self.assertNotIn("ghp_secret", text)
        self.assertTrue(cfg.describe()["admin_token"])      # says that one is set

    def test_environment_variable_overrides_the_directory(self):
        write(self.env_path, "API_BASE=https://from-env.invalid\n")
        os.environ[config.CONFIG_DIR_ENV] = self.dir
        self.addCleanup(os.environ.pop, config.CONFIG_DIR_ENV, None)
        self.assertEqual(config.load().api_base, "https://from-env.invalid")


class KeyFileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.addCleanup(self.tmp.cleanup)

    def test_reads_the_private_key_and_refuses_loose_permissions(self):
        secret = seal.generate_secret_key()
        path = os.path.join(self.dir, "admin_x25519.key")
        write(path, secret.hex() + "\n")
        cfg = config.load(self.dir)
        self.assertEqual(cfg.secret_key(), secret)
        os.chmod(path, 0o644)
        with self.assertRaises(config.ConfigError):
            cfg.secret_key()

    def test_a_missing_or_malformed_key_is_reported(self):
        cfg = config.load(self.dir)
        with self.assertRaises(config.ConfigError) as caught:
            cfg.secret_key()
        self.assertIn("keygen", str(caught.exception))
        write(os.path.join(self.dir, "admin_x25519.key"), "not hex\n")
        with self.assertRaises(config.ConfigError):
            config.load(self.dir).secret_key()


class KeygenCommandTest(unittest.TestCase):
    def run_keygen(self, home, *args):
        env = dict(os.environ)
        env["HOME"] = home
        env.pop("XDG_CONFIG_HOME", None)
        env.pop(config.CONFIG_DIR_ENV, None)
        return subprocess.run([sys.executable, CRASH_PY, "keygen", *args], capture_output=True, text=True, env=env)

    def test_writes_a_key_pair_in_a_temporary_home_and_prints_only_the_public_key(self):
        with tempfile.TemporaryDirectory() as home:
            done = self.run_keygen(home)
            self.assertEqual(done.returncode, 0, done.stderr)
            key_path = os.path.join(home, ".config", "d2vita-crash", "admin_x25519.key")
            with open(key_path, encoding="utf-8") as handle:
                secret = bytes.fromhex(handle.read().strip())
            public = seal.public_key(secret)
            self.assertIn(public.hex(), done.stdout)
            self.assertNotIn(secret.hex(), done.stdout)
            self.assertNotIn(secret.hex(), done.stderr)
            self.assertEqual(stat.S_IMODE(os.stat(key_path).st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(os.stat(os.path.dirname(key_path)).st_mode), 0o700)
            with open(os.path.join(os.path.dirname(key_path), "admin_x25519.pub"), encoding="utf-8") as handle:
                self.assertEqual(handle.read().strip(), public.hex())

    def test_refuses_to_overwrite_an_existing_key(self):
        with tempfile.TemporaryDirectory() as home:
            self.assertEqual(self.run_keygen(home).returncode, 0)
            key_path = os.path.join(home, ".config", "d2vita-crash", "admin_x25519.key")
            with open(key_path, encoding="utf-8") as handle:
                before = handle.read()
            again = self.run_keygen(home)
            self.assertEqual(again.returncode, 2)
            self.assertIn("exists", again.stderr)
            with open(key_path, encoding="utf-8") as handle:
                self.assertEqual(handle.read(), before)

    def test_json_output_carries_the_public_key_only(self):
        with tempfile.TemporaryDirectory() as home:
            done = self.run_keygen(home, "--json")
            self.assertEqual(done.returncode, 0, done.stderr)
            payload = json.loads(done.stdout)
            key_path = os.path.join(home, ".config", "d2vita-crash", "admin_x25519.key")
            with open(key_path, encoding="utf-8") as handle:
                secret = bytes.fromhex(handle.read().strip())
            self.assertEqual(payload["public_key"], seal.public_key(secret).hex())
            self.assertNotIn(secret.hex(), done.stdout)


if __name__ == "__main__":
    unittest.main()
