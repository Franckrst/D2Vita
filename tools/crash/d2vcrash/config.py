# Local configuration of the admin tool (design section 6).
#
#   ~/.config/d2vita-crash/admin.env          API_BASE, ADMIN_TOKEN, GITHUB_TOKEN, ISSUES_REPO
#   ~/.config/d2vita-crash/admin_x25519.key   X25519 private key, hex, that opens the artifacts
#   ~/.config/d2vita-crash/admin_x25519.pub   its public key, the one built into the eboot
#
# Both files must be unreadable by group and others: they hold the admin token
# and the only key that can read a player's dump. The directory is taken from
# D2VCRASH_CONFIG_DIR when set (tests), then $XDG_CONFIG_HOME, then $HOME.
import os
import re
import stat

from . import seal

CONFIG_DIR_ENV = "D2VCRASH_CONFIG_DIR"
APP_DIR_NAME = "d2vita-crash"
ENV_FILE = "admin.env"
KEY_FILE = "admin_x25519.key"
PUB_FILE = "admin_x25519.pub"
DEFAULT_ISSUES_REPO = "Franckrst/D2Vita"
KNOWN_KEYS = ("API_BASE", "ADMIN_TOKEN", "GITHUB_TOKEN", "ISSUES_REPO")

_REPO = re.compile(r"^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$")
_LINE = re.compile(r"^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")


class ConfigError(Exception):
    """The local configuration is missing, malformed or too widely readable."""


def default_dir():
    override = os.environ.get(CONFIG_DIR_ENV)
    if override:
        return override
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, APP_DIR_NAME)


def check_private(path):
    """Refuse a file that group or others can read (design section 6, 600)."""
    mode = stat.S_IMODE(os.stat(path).st_mode)
    if mode & 0o077:
        raise ConfigError(f"{path} is readable by group or others (mode {mode:04o}); run: chmod 600 {path}")


def parse_env(text, where):
    values = {}
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = _LINE.match(line)
        if not match:
            raise ConfigError(f"{where}:{number}: expected NAME=value, got {raw!r}")
        value = match.group(2).strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[match.group(1)] = value
    return values


class Config:
    """Values read from admin.env, plus the paths of the key files."""

    def __init__(self, config_dir, values):
        self.config_dir = config_dir
        self.api_base = (values.get("API_BASE") or "").rstrip("/") or None
        self.admin_token = values.get("ADMIN_TOKEN") or None
        self.github_token = values.get("GITHUB_TOKEN") or None
        self.issues_repo = values.get("ISSUES_REPO") or DEFAULT_ISSUES_REPO
        self.unknown = sorted(k for k in values if k not in KNOWN_KEYS)
        if self.api_base and not re.match(r"^https?://[!-~]+$", self.api_base):
            raise ConfigError(f"{self.env_path}: API_BASE must be an http(s) URL")
        if not _REPO.match(self.issues_repo):
            raise ConfigError(f"{self.env_path}: ISSUES_REPO must be owner/name")

    @property
    def env_path(self):
        return os.path.join(self.config_dir, ENV_FILE)

    @property
    def key_path(self):
        return os.path.join(self.config_dir, KEY_FILE)

    @property
    def pub_path(self):
        return os.path.join(self.config_dir, PUB_FILE)

    def require_api(self):
        """(api_base, admin_token), or a message naming what to add where."""
        missing = [name for name, value in (("API_BASE", self.api_base), ("ADMIN_TOKEN", self.admin_token)) if not value]
        if missing:
            raise ConfigError(f"{self.env_path}: {' and '.join(missing)} must be set "
                              f"(see tools/crash/README.md)")
        return self.api_base, self.admin_token

    def require_github(self):
        if not self.github_token:
            raise ConfigError(f"{self.env_path}: GITHUB_TOKEN must be set to create an issue")
        return self.github_token, self.issues_repo

    def secret_key(self):
        """The X25519 private key that opens sealed artifacts."""
        path = self.key_path
        if not os.path.exists(path):
            raise ConfigError(f"{path} does not exist; create one with: crash keygen")
        check_private(path)
        with open(path, encoding="utf-8") as handle:
            text = "".join(line for line in handle if not line.lstrip().startswith("#")).strip()
        try:
            key = bytes.fromhex(text)
        except ValueError as exc:
            raise ConfigError(f"{path}: expected 64 hex characters (an X25519 private key)") from exc
        if len(key) != 32:
            raise ConfigError(f"{path}: expected 64 hex characters, found {len(text)}")
        return key

    def public_key(self):
        return seal.public_key(self.secret_key())

    def describe(self):
        """What can be shown: paths and which secrets are present, never a value."""
        return {
            "config_dir": self.config_dir,
            "api_base": self.api_base,
            "admin_token": bool(self.admin_token),
            "github_token": bool(self.github_token),
            "issues_repo": self.issues_repo,
            "key_file": self.key_path if os.path.exists(self.key_path) else None,
        }

    def __repr__(self):
        return (f"Config(config_dir={self.config_dir!r}, api_base={self.api_base!r}, "
                f"admin_token={'set' if self.admin_token else 'unset'}, "
                f"github_token={'set' if self.github_token else 'unset'}, issues_repo={self.issues_repo!r})")

    __str__ = __repr__


def load(config_dir=None):
    """Read admin.env from `config_dir` (default: see default_dir)."""
    config_dir = config_dir or default_dir()
    path = os.path.join(config_dir, ENV_FILE)
    values = {}
    if os.path.exists(path):
        check_private(path)
        with open(path, encoding="utf-8") as handle:
            values = parse_env(handle.read(), path)
    return Config(config_dir, values)


def generate_key_pair(config_dir=None):
    """Write a new key pair, refusing to touch an existing one.

    Returns (public_key_hex, key_path). The private key is never returned nor
    logged: it stays in the file.
    """
    config_dir = config_dir or default_dir()
    os.makedirs(config_dir, mode=0o700, exist_ok=True)
    key_path = os.path.join(config_dir, KEY_FILE)
    pub_path = os.path.join(config_dir, PUB_FILE)
    for path in (key_path, pub_path):
        if os.path.exists(path):
            raise ConfigError(f"{path} exists; refusing to overwrite the maintainer key "
                              f"(move it away first, and keep a backup)")
    secret = seal.generate_secret_key()
    public = seal.public_key(secret)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    handle = os.open(key_path, flags, 0o600)
    try:
        os.write(handle, (secret.hex() + "\n").encode("ascii"))
    finally:
        os.close(handle)
    handle = os.open(pub_path, flags, 0o600)
    try:
        os.write(handle, (public.hex() + "\n").encode("ascii"))
    finally:
        os.close(handle)
    return public.hex(), key_path
