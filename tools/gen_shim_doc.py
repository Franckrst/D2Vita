#!/usr/bin/env python3
# tools/gen_shim_doc.py -- generates docs-site/shims.md from the SOURCES.
#
# WHY THIS SCRIPT EXISTS
# -----------------------
# A hand-written shim list rots within three commits: every extraction to
# winx86 moves dozens of keys across the engine/game boundary, and no one
# thinks to reopen the doc. A wrong list is WORSE than no list -- it leads to
# the wrong conclusion on the one question that matters here ("who serves
# this function, the engine or the game?"). So: the page is generated, and CI
# fails if it's out of date (the `doc-shims` job in .gitlab-ci.yml).
#
# WHAT IT DOES
# ------------
# It reuses tools/shim_seq.py -- already the safety net for extractions, so
# already able to reconstruct the ORDERED SEQUENCE of registrations with,
# for each one, the source unit. Ownership is inferred from the unit's path:
# under third_party/winx86/ = the ENGINE, elsewhere = the GAME.
#
# It applies the same rule as Bridge::register_shim: the LAST registration of
# a key WINS. A key registered twice is therefore not counted twice, and it's
# the last unit that gets credit. Keys with multiple registrations are listed
# separately: this is exactly the pattern that caused a real Battle.net
# connection regression (see the WinVerifyTrust comment in
# tools/rt_boot.cpp), so surfacing them is this page's main value.
#
# USAGE
#   tools/gen_shim_doc.py            # writes docs-site/shims.md
#   tools/gen_shim_doc.py --check    # writes nothing, exits 1 if the page differs
import argparse, collections, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import shim_seq  # noqa: E402

OUT = os.path.join(ROOT, 'docs-site', 'shims.md')

# Path prefix that identifies the ENGINE. Everything else is the game.
ENGINE_PREFIX = 'third_party/winx86/'

ENGINE, GAME = 'engine', 'game'


def unit_of(label):
    """'path.cpp:install_function' or 'path.cpp' -> file path."""
    return label.split(':', 1)[0]


def side_of(label):
    return ENGINE if unit_of(label).startswith(ENGINE_PREFIX) else GAME


def short_unit(label):
    """Human-readable unit name, without the full path."""
    return os.path.basename(unit_of(label))


def sort_key(key):
    """Sorts ordinals numerically, names alphabetically, ordinals last."""
    fn = key.split('!', 1)[1]
    if fn.startswith('#'):
        return (1, int(fn[1:]), '')
    return (0, 0, fn.lower())


def render(seq):
    # last registration wins, in actual call order
    winner, counts = {}, collections.Counter()
    for k, _fp, label in seq:
        winner[k] = label
        counts[k] += 1

    by_dll = collections.defaultdict(list)
    for k, label in winner.items():
        by_dll[k.split('!', 1)[0]].append((k, label))

    # A DLL is ranked by its volume, so the biggest come first.
    dlls = sorted(by_dll, key=lambda d: (-len(by_dll[d]), d.lower()))

    n_tot = len(winner)
    n_mot = sum(1 for l in winner.values() if side_of(l) == ENGINE)
    n_jeu = n_tot - n_mot

    o = []
    a = o.append
    a('# Win32 shim list\n')
    a('!!! warning "Generated page — do not hand-edit"\n')
    a('    Produced by `tools/gen_shim_doc.py` from the sources, via')
    a('    `tools/shim_seq.py`. CI regenerates this page and fails if it')
    a('    differs from what is committed. To update it:')
    a('    `tools/gen_shim_doc.py`.\n')
    a("Diablo II never calls Windows: every function imported by the game")
    a('or its DLLs is served by a native implementation. This page says')
    a('**who serves it** — the generic engine [winx86](https://winx86-136891.gitlab.io/)')
    a('(submodule `third_party/winx86`), or this repository.\n')
    a("This is the engine/game boundary seen from the facts rather than from")
    a("intent: a function is on whichever side its code is actually registered.\n")

    a('## Summary\n')
    a('| | Keys | Share |')
    a('|---|---:|---:|')
    a('| Served by the **engine** (winx86) | %d | %.0f %% |' % (n_mot, 100.0 * n_mot / n_tot))
    a('| Served by the **game** (this repository) | %d | %.0f %% |' % (n_jeu, 100.0 * n_jeu / n_tot))
    a('| **Total** | **%d** | |' % n_tot)
    a('')
    a('| DLL | Total | Engine | Game |')
    a('|---|---:|---:|---:|')
    for d in dlls:
        ks = by_dll[d]
        m = sum(1 for _, l in ks if side_of(l) == ENGINE)
        a('| `%s` | %d | %d | %d |' % (d, len(ks), m, len(ks) - m))
    a('')

    # ---- multiple registrations -------------------------------------------
    multi = sorted((k for k, n in counts.items() if n > 1), key=sort_key)
    a('## Multiple registrations\n')
    a('`Bridge::register_shim` **overwrites** the key: when a name is registered')
    a("more than once, the LAST registration wins, silently.")
    a("A losing registration is dead code that looks alive. This pattern")
    a('caused a real Battle.net connection regression — see the')
    a('`WinVerifyTrust` comment in `tools/rt_boot.cpp`.\n')
    if not multi:
        a("*No key registered more than once.*\n")
    else:
        a('| Key | Registrations | Winning unit |')
        a('|---|---:|---|')
        for k in multi:
            a('| `%s` | %d | `%s` |' % (k, counts[k], short_unit(winner[k])))
        a('')

    # ---- detail table -------------------------------------------------------
    a('## Detail by DLL\n')
    for d in dlls:
        ks = sorted(by_dll[d], key=lambda t: sort_key(t[0]))
        m = sum(1 for _, l in ks if side_of(l) == ENGINE)
        a('### `%s`\n' % d)
        a('%d keys — %d engine, %d game.\n' % (len(ks), m, len(ks) - m))
        a('| Function | Served by | Unit |')
        a('|---|---|---|')
        for k, label in ks:
            fn = k.split('!', 1)[1]
            a('| `%s` | %s | `%s` |' % (fn, side_of(label), short_unit(label)))
        a('')

    a('---\n')
    a("`native.hook` is not a DLL: these are the native replacements placed")
    a('at precise addresses of the game binary (hot-path ports).')
    a('They are specific to Diablo II by construction.')
    return '\n'.join(o) + '\n'


def require_submodule():
    """Without the winx86 sources, EVERY engine key would be classified as "game" --
    a normal-looking page that's entirely wrong on the one question it claims
    to answer. Better to refuse to produce it."""
    missing = sorted({shim_seq.UNITS[f] for f in shim_seq.UNITS
                      if shim_seq.UNITS[f].startswith(ENGINE_PREFIX)
                      and not os.path.exists(os.path.join(ROOT, shim_seq.UNITS[f]))})
    if missing:
        sys.exit('winx86 sources missing (%s...) -- the submodule is not populated.\n'
                 'Run: git submodule update --init third_party/winx86' % missing[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true',
                    help="write nothing; exit 1 if the committed page is stale")
    args = ap.parse_args()

    require_submodule()
    text = render(shim_seq.build(None, None))

    if args.check:
        try:
            with open(OUT, encoding='utf-8') as f:
                cur = f.read()
        except FileNotFoundError:
            sys.exit('docs-site/shims.md missing -- run tools/gen_shim_doc.py')
        if cur != text:
            sys.exit('docs-site/shims.md is STALE -- run tools/gen_shim_doc.py '
                     'and commit the result')
        print('docs-site/shims.md is up to date')
        return

    with open(OUT, 'w', encoding='utf-8') as f:
        f.write(text)
    print('docs-site/shims.md written')


if __name__ == '__main__':
    main()
