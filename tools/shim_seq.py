#!/usr/bin/env python3
# tools/shim_seq.py — reconstructs the ORDERED SEQUENCE of registered Win32 shims.
# Used to safety-net extractions of native hooks (D2_CELLOPT, DCC, lightgrid,
# RLE, collision...) out of tools/rt_boot.cpp.
#
# WHY THIS TOOL EXISTS
# ---------------------
# Bridge::register_shim OVERWRITES the key: when a name is registered twice,
# the LAST registration wins. Moving a block of shims — or a native hook
# installed via cpu->set_alternate()+br.shim_trap() — can therefore change the
# effective table WITHOUT breaking the build and WITHOUT anything saying so.
# This script is the check that catches it.
#
# WHAT IT DOES
# ------------
# It reads the sources and outputs, in actual call order, one line per
# registration:   <key>\t<body fingerprint>
# The key is "DLL.dll!Name" or "DLL.dll!#ordinal". The fingerprint is a sha1
# of the call's text with comments and whitespace stripped: two identical
# bodies get the same fingerprint, a modified body can't go unnoticed.
#
# The helper -> DLL mapping is NOT hardcoded: it's INFERRED from the body of
# each helper lambda (`auto K=[&](...){ ... br.register_shim(...) }`).
# A helper that registers into two DLLs — as W() does, serving both WSOCK32
# and WS2_32 — does produce two keys. `--explain` prints the inferred table.
#
# USAGE
#   tools/shim_seq.py                      # current tree
#   tools/shim_seq.py --ref <git-ref>      # state at that revision
#   tools/shim_seq.py --explain            # + inferred helper -> DLL table
# Comparing two states is done via tools/shim_seq.sh.
import argparse, hashlib, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Shim units, in the order rt_boot.cpp calls them. The REAL order is the one
# of the calls found in rt_boot.cpp, not this list's order: the list only
# says which file to open for which function.
# Populated as extractions happen: key = name of the install function called
# from main(), value = the file it lives in. rt_boot.cpp otherwise stays
# monolithic (K/U/GD/SH/A/W/Uc/REG/REGORD/GL are lambdas LOCAL to main()).
UNITS = {
    'native_hooks_codec_install_celwatch': 'src/runtime/native_hooks_codec.cpp',
    'native_hooks_codec_install_codecs':   'src/runtime/native_hooks_codec.cpp',
    'native_hooks_codec_install_post':     'src/runtime/native_hooks_codec.cpp',
    'native_hooks_cellengine_install_blit': 'src/runtime/native_hooks_cellengine.cpp',
    'native_hooks_cellengine_install_rest': 'src/runtime/native_hooks_cellengine.cpp',
    'win32_shims_shell32_d2_install':       'src/runtime/win32_shims_shell32_d2.cpp',
    # generic -> winx86 (submodule): read from disk, not via `git
    # show ref:...` (a submodule path is not a blob in the superproject's
    # history) -- harmless as long as this name doesn't appear in the
    # rt_boot.cpp of a reference revision older than its introduction, see
    # build().
    'phase_hooks_install':                   'src/runtime/phase_hooks.cpp',
    'ringtag_hooks_install':                 'src/runtime/phase_hooks.cpp',
    'cdkeys_hooks_install':                  'src/runtime/cdkeys_file.cpp',
    'win32_shims_sync_install':              'third_party/winx86/src/runtime/win32_shims_sync.cpp',
    'win32_shims_kernel32_install':          'third_party/winx86/src/runtime/win32_shims_kernel32.cpp',
    'win32_shims_locale_install':            'third_party/winx86/src/runtime/win32_shims_locale.cpp',
    'win32_shims_memory_install':            'third_party/winx86/src/runtime/win32_shims_memory.cpp',
    'win32_shims_wait_install':              'third_party/winx86/src/runtime/win32_shims_wait.cpp',
    'win32_shims_shell32_install':           'third_party/winx86/src/runtime/win32_shims_shell32.cpp',
    'win32_shims_advapi32_d2_install':      'src/runtime/win32_shims_advapi32_d2.cpp',
    'win32_shims_advapi32_install':          'third_party/winx86/src/runtime/win32_shims_advapi32.cpp',
    'win32_shims_user32_d2_install':        'src/runtime/win32_shims_user32_d2.cpp',
    'win32_shims_user32_install':            'third_party/winx86/src/runtime/win32_shims_user32.cpp',
    'win32_shims_misc_install':              'third_party/winx86/src/runtime/win32_shims_misc.cpp',
    'win32_shims_gdi32_install':              'third_party/winx86/src/runtime/win32_shims_gdi32.cpp',
    'win32_shims_window_install':             'third_party/winx86/src/runtime/win32_shims_window.cpp',
    'win32_shims_psapi_install':               'third_party/winx86/src/runtime/win32_shims_psapi.cpp',
    'win32_shims_version_install':             'third_party/winx86/src/runtime/win32_shims_version.cpp',
    'win32_shims_wsock32_install':             'third_party/winx86/src/runtime/win32_shims_wsock32.cpp',
    'win32_shims_wintrust_install':            'third_party/winx86/src/runtime/win32_shims_wintrust.cpp',
    'toolhelp_install':                    'src/runtime/toolhelp.cpp',
    'tier1_clock_cs_intrinsics_install':       'src/runtime/tier1_intrinsics_install.cpp',
    'kernel32_files_install':                  'src/runtime/kernel32_files.cpp',
    'kernel32_modules_install':                'src/runtime/kernel32_modules.cpp',
    'kernel32_time_install':                   'src/runtime/kernel32_time.cpp',
    'kernel32_interlocked_install':            'src/runtime/kernel32_interlocked.cpp',
    'kernel32_fsinfo_install':                 'src/runtime/kernel32_fsinfo.cpp',
    'kernel32_w_variants_install':             'src/runtime/kernel32_w_variants.cpp',
    'kernel32_filemapping_install':            'src/runtime/kernel32_filemapping.cpp',
    'win32_import_remainder_install':          'src/runtime/win32_import_remainder.cpp',
    'checkrevision_crypto_install':            'src/runtime/checkrevision_crypto.cpp',
    'netguard_lock_install':                   'src/runtime/netguard_lock.cpp',
}

def strip_comments(s):
    """Strips // and /* */ without touching the content of strings and char literals."""
    out=[]; i=0; n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c; out.append(c); i+=1
            while i<n:
                if s[i]=='\\': out.append(s[i:i+2]); i+=2; continue
                out.append(s[i])
                if s[i]==q: i+=1; break
                i+=1
            continue
        if c=='/' and i+1<n and s[i+1]=='/':
            while i<n and s[i]!='\n': i+=1
            continue
        if c=='/' and i+1<n and s[i+1]=='*':
            i+=2
            while i+1<n and not (s[i]=='*' and s[i+1]=='/'): i+=1
            i+=2; continue
        out.append(c); i+=1
    return ''.join(out)

def match_paren(s, i):
    """i points at '('. Returns the index of the matching ')' (strings respected)."""
    d=0; n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c; i+=1
            while i<n:
                if s[i]=='\\': i+=2; continue
                if s[i]==q: break
                i+=1
        elif c=='(': d+=1
        elif c==')':
            d-=1
            if d==0: return i
        i+=1
    raise ValueError('parenthese non fermee a %d'%i)

def match_brace(s, i):
    """i points at '{'. Returns the index of the matching '}'."""
    d=0; n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c; i+=1
            while i<n:
                if s[i]=='\\': i+=2; continue
                if s[i]==q: break
                i+=1
        elif c=='{': d+=1
        elif c=='}':
            d-=1
            if d==0: return i
        i+=1
    raise ValueError('accolade non fermee a %d'%i)

STR = r'"((?:[^"\\]|\\.)*)"'
RE_HELPER  = re.compile(r'\bauto\s+([A-Za-z_]\w*)\s*=\s*\[&?\]\s*\(')
RE_REGSHIM = re.compile(r'\bbr\.register_shim(_ordinal)?\s*\(')

def helper_targets(body, params):
    """Infers what a helper registers: list of (dll, keykind, keysrc).
    dll: literal name, or '$0' if the helper takes the DLL as its 1st parameter.
    keykind: 'name' or 'ord'. keysrc: index of the argument carrying the key."""
    dll_is_param = params and params[0][1] == 'dll'
    tgts=[]
    for m in RE_REGSHIM.finditer(body):
        ordinal = bool(m.group(1))
        op = m.end()-1
        args = split_args(body[op+1:match_paren(body, op)])
        if not args: continue
        a0 = args[0].strip()
        lit = re.fullmatch(STR, a0)
        if lit:                       dll = lit.group(1)
        elif a0 == 'dll' and dll_is_param: dll = '$0'
        else:                         continue
        # the argument carrying the key is the helper's `name`/`ord` parameter
        keyidx = 1 if dll_is_param else 0
        tgts.append((dll, 'ord' if ordinal else 'name', keyidx))
    # dedupes while keeping order (W() registers WSOCK32 then WS2_32)
    seen=set(); out=[]
    for t in tgts:
        if t in seen: continue
        seen.add(t); out.append(t)
    return out

def split_args(s):
    args=[]; d=0; cur=''; i=0; n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c; cur+=c; i+=1
            while i<n:
                if s[i]=='\\': cur+=s[i:i+2]; i+=2; continue
                cur+=s[i]
                if s[i]==q: i+=1; break
                i+=1
            continue
        if c in '([{<': d+=1
        if c in ')]}>': d-=1
        if c==',' and d==0: args.append(cur); cur=''; i+=1; continue
        cur+=c; i+=1
    if cur.strip(): args.append(cur)
    return args

def parse_params(sig):
    """['const char* name','uint32_t ac',...] -> [(type,name),...]"""
    out=[]
    for a in split_args(sig):
        a=a.strip()
        m=re.search(r'([A-Za-z_]\w*)\s*$', a)
        out.append((a, m.group(1) if m else ''))
    return out

def collect_helpers(src):
    """Returns [(def_offset, name, [targets], body_end)] for each helper."""
    hs=[]
    for m in RE_HELPER.finditer(src):
        name=m.group(1); op=m.end()-1
        try: cp=match_paren(src, op)
        except ValueError: continue
        params=parse_params(src[op+1:cp])
        bo=src.find('{', cp)
        if bo<0: continue
        try: bc=match_brace(src, bo)
        except ValueError: continue
        body=src[bo:bc+1]
        tg=helper_targets(body, params)
        if tg: hs.append((m.start(), name, tg, bc))
    return hs

def fingerprint(text):
    return hashlib.sha1(re.sub(r'\s+', '', text).encode('utf-8', 'surrogateescape')).hexdigest()[:12]

def scan_keys(src, label, helpers, explain=None):
    """Core of keys_of(): receives a list of helpers ALREADY computed, instead
    of inferring it from `src` itself. Needed for rt_boot.cpp: a unit call
    (UNITS) can land in the MIDDLE of a local helper's reuse range (K/A/U/GD...
    are defined once then called again over hundreds of lines, well past the
    first split point). Computing `helpers` once over ALL of rt_boot.cpp (see
    build()) and passing it in here keeps calls located after that split point
    from losing their helper -- which made them disappear SILENTLY from the
    table (observed in practice, not hypothetical). Returns
    [(position, key, fingerprint, label)] -- position is kept to allow
    interleaving with the units extracted in build().
    """
    if explain is not None:
        for off, name, tg, _ in helpers:
            explain.append('  %-28s %-6s -> %s' % (label, name,
                ', '.join('%s (%s, arg%d)'%(d,k,i) for d,k,i in tg)))
    # zones to ignore: the inside of a helper's body (its register_shim calls
    # there are generic, they don't name any concrete key)
    holes = [(off, end) for off, _, _, end in helpers]
    def in_hole(p): return any(a <= p <= b for a, b in holes)

    hnames = {}
    for off, name, tg, _ in helpers: hnames.setdefault(name, []).append((off, tg))

    out=[]
    # 1) helper calls
    for name, defs in hnames.items():
        for m in re.finditer(r'(?<![\w:.>])%s\s*\(' % re.escape(name), src):
            p=m.start()
            if in_hole(p): continue
            op=m.end()-1
            try: cp=match_paren(src, op)
            except ValueError: continue
            args=split_args(src[op+1:cp])
            cand=[d for d in defs if d[0] < p]
            if not cand: continue
            tg=max(cand, key=lambda d: d[0])[1]
            fp=fingerprint(src[p:cp+1])
            for dll, kind, keyidx in tg:
                if dll=='$0':
                    lit=re.fullmatch(STR, args[0].strip())
                    if not lit: continue
                    dllv=lit.group(1)
                else:
                    dllv=dll
                if keyidx>=len(args): continue
                a=args[keyidx].strip()
                if kind=='ord':
                    mm=re.fullmatch(r'(?:0[xX])?[0-9a-fA-F]+[uU]?', a)
                    if not mm: continue
                    key='%s!#%d' % (dllv, int(a.rstrip('uU'), 0))
                else:
                    lit=re.fullmatch(STR, a)
                    if not lit: continue
                    key='%s!%s' % (dllv, lit.group(1))
                out.append((p, key, fp, label))
    # 2) hand-written registrations (DSOUND, wsprintfA, WINMM...)
    for m in RE_REGSHIM.finditer(src):
        p=m.start()
        if in_hole(p): continue
        ordinal=bool(m.group(1)); op=m.end()-1
        try: cp=match_paren(src, op)
        except ValueError: continue
        args=split_args(src[op+1:cp])
        if len(args)<2: continue
        d=re.fullmatch(STR, args[0].strip())
        if not d: continue
        a=args[1].strip()
        if ordinal:
            if not re.fullmatch(r'(?:0[xX])?[0-9a-fA-F]+[uU]?', a): continue
            key='%s!#%d' % (d.group(1), int(a.rstrip('uU'), 0))
        else:
            lit=re.fullmatch(STR, a)
            if not lit: continue
            key='%s!%s' % (d.group(1), lit.group(1))
        # a manual registration's fingerprint is that of the call itself
        out.append((p, key, fingerprint(src[p:cp+1]), label))
    out.sort(key=lambda t: t[0])
    return out

def keys_of(src, label, explain=None):
    """Ordered sequence of registrations for a unit (extracted file), already
    stripped of its comments: its helpers are local to `src`, so they're
    inferred from `src` itself (unlike rt_boot.cpp, see scan_keys).
    Returns [(key, fingerprint, label)]."""
    helpers = collect_helpers(src)
    return [(k, f, l) for _, k, f, l in scan_keys(src, label, helpers, explain)]

# Submodules known to UNITS: a path starting with one of these is NOT a blob
# in the superproject's history (it's a gitlink), so `git show <ref>:<path>`
# ALWAYS fails, even when the file does exist in the submodule at the
# recorded revision. The submodule's SHA at THAT ref must first be found
# (git ls-tree), then read from within the submodule's own repo. Needed as
# soon as a winx86 extraction (e.g. win32_shims_shell32_install) is called
# from a rt_boot.cpp revision being compared via --ref.
SUBMODULES = ['third_party/winx86']

def submodule_sha_at(ref, subpath):
    r = subprocess.run(['git', '-C', ROOT, 'ls-tree', ref, subpath], capture_output=True)
    if r.returncode != 0: return None
    out = r.stdout.decode('utf-8', 'surrogateescape').strip()
    if not out: return None
    parts = out.split()
    if len(parts) < 3 or parts[1] != 'commit': return None
    return parts[2]

def read(path, ref):
    if ref:
        for sm in SUBMODULES:
            if path == sm or path.startswith(sm + '/'):
                sha = submodule_sha_at(ref, sm)
                if sha is None: return None
                rel = path[len(sm)+1:]
                r = subprocess.run(['git', '-C', os.path.join(ROOT, sm), 'show', '%s:%s' % (sha, rel)],
                                   capture_output=True)
                if r.returncode != 0: return None
                return r.stdout.decode('utf-8', 'surrogateescape')
        r = subprocess.run(['git', '-C', ROOT, 'show', '%s:%s' % (ref, path)],
                           capture_output=True)
        if r.returncode != 0: return None
        return r.stdout.decode('utf-8', 'surrogateescape')
    p = os.path.join(ROOT, path)
    if not os.path.exists(p): return None
    return open(p, encoding='utf-8', errors='surrogateescape').read()

def unit_body(src, fn):
    # The signature isn't fixed to any particular shape -- d2-vita extracts
    # install(Cpu*,Bridge&[,...]); only the function NAME identifies the
    # unit, the parameter list is free.
    m = re.search(r'\bvoid\s+%s\s*\(' % re.escape(fn), src)
    if not m: return None
    bo = src.find('{', m.end())
    return src[bo:match_brace(src, bo)+1]

def build(ref, explain=None):
    rt = read('tools/rt_boot.cpp', ref)
    if rt is None: sys.exit('tools/rt_boot.cpp introuvable (ref=%s)' % ref)
    rt = strip_comments(rt)
    # helpers computed ONCE over the whole of rt_boot.cpp (not piecewise): a
    # unit call (UNITS) can land in the middle of a local helper's reuse
    # range (K/A/U/GD... live until the end of main()) -- see the note on
    # scan_keys().
    helpers = collect_helpers(rt)
    full = scan_keys(rt, 'tools/rt_boot.cpp', helpers, explain)   # [(pos,key,fingerprint,label)]
    cache = {}
    seq = []
    fi = 0
    # splits rt_boot.cpp at register_* calls, and interleaves the sequence of
    # the unit called AT THAT POINT: register_misc and register_coverage are
    # each called from two distinct places, with DSOUND/GDI32/USER32 in between.
    # Accepts an arbitrary argument list (no nested parentheses in real calls),
    # not just a single identifier -- d2-vita calls install(cpu,br[,&g_frame]).
    # The scan below only recognizes units DECLARED in UNITS. A forgotten unit
    # isn't flagged -- its registrations simply vanish from the count, and the
    # total silently drops. A net that miscounts is worse than no net, so this
    # fails loudly instead of silently undercounting.
    check_declared(rt, 'rt_boot.cpp')
    for off, fn in unit_calls(rt) + [(len(rt), None)]:
        while fi < len(full) and full[fi][0] < off:
            seq.append(full[fi][1:]); fi += 1
        if fn is None: break
        seq += unit_seq(fn, ref, cache, 'rt_boot.cpp', explain)
    return seq

def check_declared(src, where):
    seen = set(m.group(1) for m in
               re.finditer(r'\b([A-Za-z_]\w*_install)\s*\([^()]*\)\s*;', src))
    missing = sorted(seen - set(UNITS))
    if missing:
        sys.exit('shim_seq: %s appelle %d unite(s) ABSENTE(S) de UNITS : %s\n'
                 "  Leurs inscriptions seraient omises EN SILENCE et le total "
                 "paraitrait avoir baisse.\n"
                 '  Ajoute-les dans UNITS (tools/shim_seq.py) avec le chemin de '
                 'leur fichier.'
                 % (where, len(missing), ', '.join(missing)))

def unit_calls(src):
    return ([(m.start(), m.group(1)) for m in
             re.finditer(r'\b(%s)\s*\([^()]*\)\s*;' % '|'.join(UNITS), src)]
            if UNITS else [])

def unit_seq(fn, ref, cache, caller, explain, stack=()):
    # A unit's registrations in order, with any unit IT calls expanded at the
    # call site: an extracted module may itself call another installer.
    path = UNITS[fn]
    if path not in cache:
        u = read(path, ref)
        cache[path] = strip_comments(u) if u is not None else None
    u = cache[path]
    if u is None:
        sys.exit('%s appelle %s mais %s est absent (ref=%s)' % (caller, fn, path, ref))
    body = unit_body(u, fn)
    if body is None: sys.exit('%s introuvable dans %s' % (fn, path))
    label = path + ':' + fn
    check_declared(body, label)
    regs = scan_keys(body, label, collect_helpers(body), explain)
    out, ri = [], 0
    for off, sub in unit_calls(body) + [(len(body), None)]:
        while ri < len(regs) and regs[ri][0] < off:
            out.append(regs[ri][1:]); ri += 1
        if sub is None: break
        if sub == fn or sub in stack:
            sys.exit('shim_seq: appel recursif de %s depuis %s' % (sub, label))
        out += unit_seq(sub, ref, cache, label, explain, stack + (fn,))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', default=None, help='revision git a lire (defaut: arbre courant)')
    ap.add_argument('--explain', action='store_true', help='imprime la table helper -> DLL deduite')
    a = ap.parse_args()
    ex = [] if a.explain else None
    seq = build(a.ref, ex)
    if ex:
        sys.stderr.write('# table helper -> DLL, DEDUITE des sources :\n')
        sys.stderr.write('\n'.join(ex) + '\n#\n')
    for k, f, _ in seq: print('%s\t%s' % (k, f))
    sys.stderr.write('# %d inscriptions\n' % len(seq))

if __name__ == '__main__':
    main()
