# Autopsy of a stored dump: open it, then run tools/autopsie_psp2dmp.py on it.
#
# The sealed dump is downloaded and opened with the private key (nothing else
# can read it), then handed to the reference postmortem script with everything
# that makes its output true for that build:
#   * the symbols archived after the build, ~/d2vita-symbols/<build_id>/
#     (nm.txt, d2vita.elf), design section 4.10;
#   * the arena host base and the runtime address of main, both printed in the
#     boot_progress of the same report when it was uploaded.
import os
import re
import subprocess
import sys

SYMBOLS_DIR_ENV = "D2VCRASH_SYMBOLS_DIR"
# Defaults of tools/autopsie_psp2dmp.py, repeated so the command line always
# says which bases were used.
DEFAULT_GAME_BASE = 0x02100000   # 0x01900000 for a report from 0.1.6 or earlier
DEFAULT_ARENA_HOST_BASE = 0x84000000
DEFAULT_SYMBOLS_DIR = "~/d2vita-symbols"
# d2vcrash/ -> tools/crash/ -> tools/ -> the repository root.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SCRIPT = os.path.join(REPO_ROOT, "tools", "autopsie_psp2dmp.py")

# winx86 prints "arene: base=0x84300000 membase=0x85000000 ..."; membase is the
# value added to a guest address to reach the host view of the arena.
_MEMBASE = re.compile(r"\bmembase=0x([0-9a-fA-F]+)")
# rt_boot.cpp: "module: main a l'execution=0x81024730 (biais = cette valeur - nm(main))"
_MAIN = re.compile(r"module: main a l'execution=0x([0-9a-fA-F]+)")
# claim.v1#BuildId: <VERSION>+<12 lowercase hex digits>[-dirty].
BUILD_ID = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+\+[0-9a-f]{12}(-dirty)?\Z")


def symbols_root(explicit=None):
    return explicit or os.environ.get(SYMBOLS_DIR_ENV) or os.path.expanduser(DEFAULT_SYMBOLS_DIR)


def symbols_for(build_id, root=None):
    """{"nm": ..., "elf": ...} for the files archived for that build, if any.

    The build id comes from a claim, so it names one directory of the symbol
    root or nothing at all: a value that is not a build id looks nowhere.
    """
    if not build_id or not BUILD_ID.match(build_id):
        return {}
    directory = os.path.join(symbols_root(root), build_id)
    found = {}
    for key, name in (("nm", "nm.txt"), ("elf", "d2vita.elf")):
        path = os.path.join(directory, name)
        if os.path.exists(path):
            found[key] = path
    return found


def scan_boot_progress(text):
    """What the boot log says about the bases of that run."""
    facts = {}
    membase = _MEMBASE.search(text)
    if membase:
        facts["arena_host_base"] = int(membase.group(1), 16)
    main = _MAIN.search(text)
    if main:
        facts["main_runtime"] = int(main.group(1), 16)
    return facts


def read_boot_progress(directory):
    path = os.path.join(directory, "boot_progress.txt")
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8", errors="replace") as handle:
        return scan_boot_progress(handle.read())


def build_command(dump_path, game_base, arena_host_base, symbols=None, main_runtime=None, python=None):
    """The argv of the reference postmortem for that dump."""
    argv = [python or sys.executable, SCRIPT, dump_path,
            "--game-base", hex(game_base), "--arena-host-base", hex(arena_host_base)]
    symbols = symbols or {}
    if symbols.get("nm"):
        argv += ["--nm", symbols["nm"]]
        # The eboot is not loaded at a fixed address: without a bias the script
        # prints no symbol rather than a wrong name.
        if main_runtime is not None:
            argv += ["--main-runtime", hex(main_runtime)]
        elif symbols.get("elf"):
            argv += ["--elf", symbols["elf"]]
    elif symbols.get("elf"):
        argv += ["--elf", symbols["elf"]]
    return argv


def run(argv, timeout=600):
    """(exit code, output) of the postmortem script.

    A run that overruns is reported like any other failure, with 124 as the
    shell's `timeout` uses: TimeoutExpired is not an OSError, so uncaught it
    would leave a Python traceback on the command line and in the web UI.
    """
    try:
        done = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as expired:
        partial = "".join(piece.decode("utf-8", "replace") if isinstance(piece, bytes) else piece
                          for piece in (expired.stdout, expired.stderr) if piece)
        return 124, f"{partial}the postmortem ran longer than {timeout} s and was stopped: {SCRIPT}\n"
    return done.returncode, done.stdout + done.stderr
