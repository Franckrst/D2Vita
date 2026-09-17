# tools/crash/tests/support.py — paths and shared helpers for the admin tests.
#
# Every test module starts with the two sys.path lines below, so the suite runs
# both as `python3 -m unittest discover -s tools/crash/tests` and from any
# working directory.
import os
import sys

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
CRASH_DIR = os.path.dirname(TESTS_DIR)               # tools/crash
REPO_ROOT = os.path.dirname(os.path.dirname(CRASH_DIR))
VECTOR_DIR = os.path.join(REPO_ROOT, "tests", "crashreport", "vectors")
# The contract itself is read-only and lives outside this repository; the
# vectors above are its copy (tools/tests/sync_contract_vectors.sh).
CONTRACT_DIR = os.environ.get("D2V_CONTRACT") or os.path.expanduser("~/repos/D2Vita-website/contract")
SCHEMA_DIR = os.path.join(CONTRACT_DIR, "schemas")


def add_paths():
    for path in (TESTS_DIR, CRASH_DIR):
        if path not in sys.path:
            sys.path.insert(0, path)


add_paths()

CRASH_PY = os.path.join(CRASH_DIR, "crash.py")


def write_config(config_dir, base_url, token, **values):
    """An admin.env and a fresh key pair in a throwaway directory."""
    from d2vcrash import config

    os.makedirs(config_dir, mode=0o700, exist_ok=True)
    lines = [f"API_BASE={base_url}", f"ADMIN_TOKEN={token}"]
    lines += [f"{name}={value}" for name, value in values.items()]
    path = os.path.join(config_dir, config.ENV_FILE)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    os.chmod(path, 0o600)
    public_key, _ = config.generate_key_pair(config_dir)
    return bytes.fromhex(public_key)


def run_crash(config_dir, *args, stdin=None, env=None):
    """Run tools/crash/crash.py as a session would."""
    import subprocess

    from d2vcrash import config

    environment = dict(os.environ)
    environment[config.CONFIG_DIR_ENV] = config_dir
    environment.update(env or {})
    return subprocess.run([sys.executable, CRASH_PY, *args], capture_output=True, text=True,
                          input=stdin, env=environment, timeout=120)


def store_artifact(dataset, report_id, name, plaintext, recipient_pk, in_sample=True):
    """Seal `plaintext` and register it as a stored artifact of that report."""
    import hashlib

    from d2vcrash import seal

    sealed = seal.seal(plaintext, recipient_pk)
    dataset.artifacts[(report_id, name)] = sealed
    entry = {"name": name, "bytes": len(sealed), "sha256": hashlib.sha256(sealed).hexdigest(),
             "stored_unix": 1789284100}
    report = dataset.reports[report_id]
    report["artifacts"] = [a for a in report["artifacts"] if a["name"] != name] + [entry]
    if in_sample:
        for detail in dataset.signatures.values():
            if detail.get("sample_report") == report_id:
                detail["sample_artifacts"] = [a for a in detail["sample_artifacts"] if a["name"] != name] + [entry]
    return sealed
