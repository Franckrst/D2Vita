#!/usr/bin/env python3
# tools/autopsie_psp2dmp.py — automatic postmortem for a native fault.
#
# A native fault kills the process without writing a single line to the log:
# the psp2dmp is the only trace available.
#
#   python3 tools/autopsie_psp2dmp.py core.psp2dmp
#       [--game-base 0x01900000] [--arena-host-base 0x84000000]
#       [--nm ~/d2vita-symbols/<build_id>/nm.txt] [--main-runtime 0x81024730]
#       [--elf ~/d2vita-symbols/<build_id>/d2vita.elf] [--first-thread]
#
# Output:
#   - the thread list (THREAD_INFO), the faulting one marked;
#   - registers of the faulting thread;
#   - their translation to GUEST registers via the box86 map
#     (xEAX=r4 xECX=r5 xEDX=r6 xEBX=r7 xESP=r8 xEBP=r9 xESI=r10 xEDI=r11);
#   - the module and, with --nm, the symbol of PC and LR;
#   - the disassembled faulting ARM instruction, if the JIT cache is in the dump;
#   - the GUEST CALL CHAIN, walked via chained EBP frames on the guest
#     stack (it IS in the dump: host = guest + the arena host base).
#
# The faulting thread is the first THREAD_INFO entry whose stop reason is not
# zero, else the first registered thread (same rule as
# src/crashreport/cr_psp2dmp.cpp, which derived the note offsets from the
# maintainer's real dumps); --first-thread keeps the older rule.
#
# Symbols. The eboot is NOT loaded at a fixed address (rt_boot.cpp publishes
# "module: main a l'execution=..." for exactly this reason), so a symbol name
# is only printed once the bias is known: from --main-runtime (that log line),
# from --eboot-bias, or from the dump's module list together with the link
# address of --elf. Without any of them, --nm alone prints no symbol.
import argparse
import gzip
import os
import struct
import subprocess
import sys
import tempfile

GAME_BASE = 0x01900000      # Game.exe load base in the guest arena
PE_IMAGE_BASE = 0x400000    # its PE image base: VA = address - load base + this
CODE_SPAN = 0x400000        # a return address of Game.exe lies in [base, base + span)
ARENA_HOST_BASE = 0x84000000   # host view of the guest arena (varies by boot: a real log has shown membase 0x85000000 too)
ARENA = 0x12900000          # arena size: beyond this, a "pointer" is noise
MAX_FRAMES = 24
MAX_LINK = 0x10000
BOX86 = {4: 'EAX', 5: 'ECX', 6: 'EDX', 7: 'EBX', 8: 'ESP', 9: 'EBP', 10: 'ESI', 11: 'EDI'}


def load(path):
    with open(path, 'rb') as handle:
        data = handle.read()
    if data[:2] == b'\x1f\x8b':
        try:
            data = gzip.decompress(data)
        except (OSError, EOFError) as exc:
            # Two real dumps end inside their gzip member; keep what inflated.
            data = _inflate_partial(data)
            if not data:
                sys.exit('gzip illisible: %s' % exc)
            print('(gzip tronque: %d octets recuperes)' % len(data))
    if data[:4] != b'\x7fELF':
        sys.exit('pas un ELF (ni un gzip d ELF)')
    return data


def _inflate_partial(data):
    import zlib
    stream = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        return stream.decompress(data)
    except zlib.error:
        return b''


def notes_in(data, off, size):
    """Notes of one PT_NOTE segment, tolerating a segment cut short or absent.

    Real dumps announce a second note group after the memory, and two of them
    end before it: a note past the end of the file is skipped, never fatal.
    """
    out = {}
    end = min(off + size, len(data))
    p = off
    while p + 12 <= end:
        nsz, dsz, _ntype = struct.unpack_from('<3I', data, p)
        name_end = p + 12 + min(nsz, 64)
        if name_end > end:
            break
        name = data[p + 12:p + 12 + nsz].split(b'\0')[0].decode('latin1', 'replace')
        dp = p + 12 + ((nsz + 3) & ~3)
        if dp + dsz > end:
            break
        out[name] = (dp, dsz)
        p = dp + ((dsz + 3) & ~3)
    return out


def parse(data):
    """(loads, notes, truncated): PT_LOAD list, notes by name, short file flag.

    Every read is bounded: a dump can be cut anywhere (two real ones are), and
    a damaged header must give a message, not a traceback.
    """
    phoff, = struct.unpack_from('<I', data, 0x1c)
    phe, phn = struct.unpack_from('<HH', data, 0x2a)
    loads, notes, truncated = [], {}, False
    if phe < 32:
        print('(en-tete ELF inattendu: phentsize=%d)' % phe)
        return loads, notes, True
    for i in range(phn):
        entry = phoff + i * phe
        if entry + 32 > len(data):
            truncated = True                # the program header table is cut short
            break
        typ, off, va, _pa, fsz, _msz, _fl, _al = struct.unpack_from('<8I', data, entry)
        if off + fsz > len(data):
            truncated = True            # announced past the end of the file
        if typ == 1:
            loads.append((va, fsz, off))
        elif typ == 4:
            if off >= len(data):
                continue                # note group entirely missing
            notes.update(notes_in(data, off, fsz))
    loads.sort()
    return loads, notes, truncated


def mk_read(data, loads):
    def rd(va, n):
        for base, fsz, off in loads:
            if base <= va and va + n <= base + fsz:
                start = off + (va - base)
                if start + n > len(data):
                    return None         # segment announced but not in the file
                return data[start:start + n]
        return None
    return rd


def thread_regs(data, notes):
    """THREAD_REG_INFO: size, tid, r0..r12, sp, lr, pc, cpsr."""
    if 'THREAD_REG_INFO' not in notes:
        return []
    dp, dsz = notes['THREAD_REG_INFO']
    end = min(dp + dsz, len(data))
    p, out = dp + 8, []
    while p + 4 <= end:
        size, = struct.unpack_from('<I', data, p)
        if size < 76 or p + size > end:
            break
        w = struct.unpack_from('<19I', data, p)
        out.append(dict(tid=w[1], r=list(w[2:15]), sp=w[15], lr=w[16], pc=w[17], cpsr=w[18]))
        p += size
    return out


def thread_info(data, notes):
    """THREAD_INFO: uid, name, status, stop reason, pc (offsets of cr_psp2dmp.cpp)."""
    if 'THREAD_INFO' not in notes:
        return []
    dp, dsz = notes['THREAD_INFO']
    end = min(dp + dsz, len(data))
    p, out = dp + 8, []
    while p + 8 <= end:
        size, = struct.unpack_from('<I', data, p)
        if size < 0xa0 or p + size > end:
            break
        out.append(dict(uid=struct.unpack_from('<I', data, p + 4)[0],
                        name=data[p + 8:p + 0x28].split(b'\0')[0].decode('latin1', 'replace'),
                        status=struct.unpack_from('<I', data, p + 0x30)[0] & 0xffff,
                        stop=struct.unpack_from('<I', data, p + 0x74)[0],
                        pc=struct.unpack_from('<I', data, p + 0x9c)[0]))
        p += size
    return out


def modules(data, notes):
    """MODULE_INFO: uid, name and segments (va, size) of every loaded module."""
    if 'MODULE_INFO' not in notes:
        return []
    dp, dsz = notes['MODULE_INFO']
    end = min(dp + dsz, len(data))
    if dp + 8 > end:
        return []
    count, = struct.unpack_from('<I', data, dp + 4)
    out, p = [], dp + 8
    for _ in range(min(count, 256)):
        if p + 0x50 > end:
            break
        nseg, = struct.unpack_from('<I', data, p + 0x4c)
        if nseg > 16 or p + 0x50 + nseg * 0x14 + 0x10 > end:
            break
        segs = []
        for k in range(nseg):
            sp = p + 0x50 + k * 0x14
            va, size = struct.unpack_from('<II', data, sp + 8)
            segs.append((va, size))
        out.append(dict(uid=struct.unpack_from('<I', data, p + 4)[0],
                        name=data[p + 0x24:p + 0x44].split(b'\0')[0].decode('latin1', 'replace'),
                        segs=segs))
        p += 0x50 + nseg * 0x14 + 0x10
    return out


def module_of(mods, addr):
    """(name, offset in the segment) of the module holding `addr`, or None."""
    for module in mods:
        for va, size in module['segs']:
            if size and va <= addr < va + size:
                return module['name'], addr - va
    return None


def main_module(mods):
    """The application's own ELF: the lowest uid in every real dump examined."""
    return min(mods, key=lambda m: m['uid']) if mods else None


def choose_thread(regs, infos, first_only=False):
    """Index in `regs` of the faulting thread (cr_psp2dmp.cpp, ThreadRule)."""
    if first_only or not regs:
        return 0
    for info in infos:
        if info['stop'] == 0:
            continue
        for index, reg in enumerate(regs):
            if reg['tid'] == info['uid']:
                return index
        break                            # named but not registered: keep the first
    return 0


def load_nm(path):
    """Sorted (address, name) of an `arm-vita-eabi-nm d2vita.elf` file."""
    symbols = []
    with open(path, encoding='utf-8', errors='replace') as handle:
        for line in handle:
            parts = line.split()
            if len(parts) < 3 or len(parts[0]) < 8:
                continue                 # undefined or weak: "         w name"
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            symbols.append((addr, parts[-1]))
    symbols.sort()
    return symbols


def symbolize(symbols, addr, bias):
    """"name+0xoff" for a runtime address, or None outside every symbol."""
    if not symbols:
        return None
    target = addr - bias
    low, high = 0, len(symbols)
    while low < high:
        middle = (low + high) // 2
        if symbols[middle][0] <= target:
            low = middle + 1
        else:
            high = middle
    if low == 0:
        return None
    base, name = symbols[low - 1]
    return '%s+0x%x' % (name, target - base)


def eboot_bias(args, symbols, mods):
    """(bias, how it was found) between runtime eboot addresses and the ELF."""
    if args.eboot_bias is not None:
        return args.eboot_bias, 'option --eboot-bias'
    if args.main_runtime is not None:
        for addr, name in symbols:
            if name == 'main':
                return args.main_runtime - addr, 'main a l execution - nm(main)'
        sys.exit('--main-runtime donne mais nm ne contient pas "main"')
    if args.elf:
        module = main_module(mods)
        if module and module['segs']:
            with open(args.elf, 'rb') as handle:
                head = handle.read(0x400)
            if head[:4] != b'\x7fELF':
                sys.exit('%s n est pas un ELF' % args.elf)
            phoff, = struct.unpack_from('<I', head, 0x1c)
            phe, phn = struct.unpack_from('<HH', head, 0x2a)
            link = None
            for i in range(phn):
                typ, _off, va = struct.unpack_from('<3I', head, phoff + i * phe)
                if typ == 1 and (link is None or va < link):
                    link = va
            if link is not None:
                return module['segs'][0][0] - link, 'MODULE_INFO(%s) - PT_LOAD(%s)' % (
                    module['name'], os.path.basename(args.elf))
    return None, None


def describe(addr, mods, symbols, bias):
    """" module+0xoff (sym: name+0xoff)" for a host address."""
    parts = []
    where = module_of(mods, addr)
    if where:
        parts.append('%s+0x%x' % where)
    if symbols and bias is not None:
        name = symbolize(symbols, addr, bias)
        if name:
            parts.append('sym: %s' % name)
    return '  [%s]' % ', '.join(parts) if parts else ''


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description='autopsie d un psp2dmp (faute native)')
    ap.add_argument('dump')
    ap.add_argument('--game-base', type=lambda v: int(v, 0), default=GAME_BASE,
                    help='base de chargement de Game.exe (defaut 0x%x)' % GAME_BASE)
    ap.add_argument('--arena-host-base', type=lambda v: int(v, 0), default=ARENA_HOST_BASE,
                    help='vue hote de l arene invitee, "membase" du journal (defaut 0x%x)' % ARENA_HOST_BASE)
    ap.add_argument('--nm', help='table des symboles de l eboot (arm-vita-eabi-nm)')
    ap.add_argument('--elf', help='d2vita.elf du meme build (pour le biais)')
    ap.add_argument('--main-runtime', type=lambda v: int(v, 0),
                    help='adresse de main a l execution (ligne "module: main a l execution" du journal)')
    ap.add_argument('--eboot-bias', type=lambda v: int(v, 0), help='biais deja connu')
    ap.add_argument('--first-thread', action='store_true',
                    help='prendre le premier fil enregistre, sans lire les raisons d arret')
    ap.add_argument('--exe', help='(reserve) Game.exe de reference')
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    game_base = args.game_base
    host = args.arena_host_base
    data = load(args.dump)
    loads, notes, truncated = parse(data)
    rd = mk_read(data, loads)
    regs = thread_regs(data, notes)
    infos = thread_info(data, notes)
    mods = modules(data, notes)
    if not regs:
        sys.exit('aucun fil dans THREAD_REG_INFO')
    if truncated:
        print('(fichier tronque: des segments annonces manquent a la fin)')
    symbols = load_nm(args.nm) if args.nm else []
    bias, how = eboot_bias(args, symbols, mods)
    if symbols:
        if bias is None:
            print('(symboles lus mais biais inconnu: donnez --main-runtime, --eboot-bias ou --elf)')
        else:
            print('(symboles: %d, biais=0x%x, %s)' % (len(symbols), bias, how))

    index = choose_thread(regs, infos, args.first_thread)
    t = regs[index]
    by_uid = {info['uid']: info for info in infos}

    if infos:
        print('== FILS (THREAD_INFO)')
        for info in infos:
            mark = '   <-- FAUTIF' if info['uid'] == t['tid'] else ''
            print('   %#010x %-20s status=%d stop=%#x pc=%08x%s'
                  % (info['uid'], info['name'], info['status'], info['stop'], info['pc'], mark))
        print('')

    faulting = by_uid.get(t['tid'])
    print('== FIL FAUTIF tid=%#x%s' % (t['tid'], (' (%s)' % faulting['name']) if faulting else ''))
    print('   pc=%08x lr=%08x sp=%08x cpsr=%08x' % (t['pc'], t['lr'], t['sp'], t['cpsr']))
    if faulting:
        print('   raison d arret=%#x' % faulting['stop'])
    print('   pc%s' % (describe(t['pc'], mods, symbols, bias) or '  [hors module connu: cache JIT ou tas]'))
    print('   lr%s' % (describe(t['lr'], mods, symbols, bias) or '  [hors module connu: cache JIT ou tas]'))
    print('   registres INVITES (carte box86) :')
    for i, nm in sorted(BOX86.items()):
        v = t['r'][i]
        note = ''
        if nm not in ('ESP', 'EBP') and not (0 < v < ARENA):
            note = '  <-- HORS ARENE'
        print('     %s = %08x%s' % (nm, v, note))
    if game_base <= t['lr'] < game_base + CODE_SPAN:
        print('   lr pointe le code invite : Game+0x%06x' % (t['lr'] - game_base + PE_IMAGE_BASE))

    blob = rd(t['pc'] - 0x20, 0x40) if t['pc'] >= 0x20 else None
    if blob:
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(blob)
            tmp = f.name
        print('\n== CACHE JIT autour du PC')
        try:
            out = subprocess.run(['arm-linux-gnueabihf-objdump', '-D', '-b', 'binary', '-m', 'arm',
                                  '--adjust-vma=%d' % (t['pc'] - 0x20), tmp],
                                 capture_output=True, text=True).stdout
            for ln in out.splitlines():
                if ':\t' in ln:
                    print(('  >>> ' if ('%x:' % t['pc']) in ln else '      ') + ln.strip())
        except FileNotFoundError:
            print('  (arm-linux-gnueabihf-objdump absent)')
        finally:
            os.unlink(tmp)
    else:
        print('\n(cache JIT absent du dump : pas de desassemblage)')

    print('\n== CHAINE D APPEL INVITEE (cadres EBP chaines)')
    ebp = t['r'][9]
    for _ in range(MAX_FRAMES):
        fr = rd(host + ebp, 8)
        if not fr:
            print('   (cadre %08x hors dump)' % ebp)
            break
        nxt, ra = struct.unpack('<2I', fr)
        if not (game_base <= ra < game_base + CODE_SPAN):
            break
        print('   VA 0x%06x   (RVA 0x%06x, pour set_alternate)   cadre=%08x'
              % (ra - game_base + PE_IMAGE_BASE, ra - game_base, ebp))
        if nxt <= ebp or nxt - ebp > MAX_LINK:
            break
        ebp = nxt
    return 0


if __name__ == '__main__':
    sys.exit(main())
