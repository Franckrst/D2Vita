#!/usr/bin/env python3
# tools/tests/gen_fake_psp2dmp.py — synthetic PS Vita core dumps (psp2dmp) for
# tools/tests/crashreport_test.cpp and the autopsie oracle.
#
#   python3 tools/tests/gen_fake_psp2dmp.py --out DIR
#
# Real dumps can hold CD keys, so they never enter the repository; these files
# reproduce the layout observed empirically in real dumps:
#   * gzip of an ELF32 little-endian ARM core (e_type 4, e_flags 0x05000000);
#   * a first group of PT_NOTE segments (one note each, 16-byte name field,
#     namesz = 16), then PT_LOAD segments, then a second group of notes
#     (MEM_BLK_INFO ... TTY_INFO, a 4 MiB SYSTEM_INFO2, SUMMARY_INFO), all in
#     increasing file offset;
#   * THREAD_INFO: u32 0x12, u32 count, entries of 0xc8 bytes
#     (+0x04 uid, +0x08 name[32], +0x30 status, +0x74 stop reason, +0x9c pc);
#   * THREAD_REG_INFO: u32 0x11, u32 count, entries of 0x178 bytes
#     (size, tid, r0..r12, sp, lr, pc, cpsr — the layout read by
#     tools/autopsie_psp2dmp.py:55-64);
#   * MODULE_INFO: u32 1, u32 count, modules (+0x04 uid, +0x24 name[32],
#     +0x4c segment count, 0x14-byte segments {0, attr, vaddr, memsz, align},
#     then 0x10 bytes);
#   * APP_INFO title id at +0x08, SYSTEM_INFO firmware (0x03650011) at +0x08.
# The stamp note (D2VITA_STAMP) is synthetic: the real sceCoredumpWriteUserData
# layout is still to be observed; the reader looks for the marker in the
# bytes of every PT_NOTE segment, never in memory.
#
# Guest constants match tools/autopsie_psp2dmp.py: arena host view 0x84000000,
# Game.exe loaded at 0x01900000 (the script's GB 0x01500000 + 0x400000).
import argparse
import gzip
import os
import struct

HOST = 0x84000000
GAME = 0x01900000
JIT = 0x96900000
EBOOT_TEXT = 0x81000000
SESSION_ID = '0123456789abcdef0123456789abcdef'
BUILD_ID = '0.1.0+ab12cd34ef56'

NT_COREFILE, NT_SYSTEM, NT_PROCESS, NT_THREAD, NT_THREAD_REG, NT_MODULE, NT_APP = 4096, 4097, 4098, 4099, 4100, 4101, 4124


def note(name, data, ntype):
    nm = name.encode().ljust(16, b'\0')
    return struct.pack('<3I', 16, len(data), ntype) + nm + data + b'\0' * ((-len(data)) % 4)


def cstr(s, n):
    b = s.encode()[:n - 1]
    return b + b'\0' * (n - len(b))


def thread_info(threads):
    out = struct.pack('<II', 0x12, len(threads))
    for t in threads:
        e = bytearray(0xc8)
        struct.pack_into('<II', e, 0, 0xc8, t['uid'])
        e[0x08:0x28] = cstr(t['name'], 32)
        struct.pack_into('<I', e, 0x30, t['status'])
        struct.pack_into('<I', e, 0x74, t['stop'])
        struct.pack_into('<I', e, 0x9c, t['pc'])
        out += bytes(e)
    return out


def thread_reg_info(threads):
    out = struct.pack('<II', 0x11, len(threads))
    for t in threads:
        e = bytearray(0x178)
        r = t['r']
        struct.pack_into('<19I', e, 0, 0x178, t['uid'], *r, t['sp'], t['lr'], t['pc'], t['cpsr'])
        out += bytes(e)
    return out


def module_info(mods):
    out = struct.pack('<II', 1, len(mods))
    for m in mods:
        e = bytearray(0x50)
        struct.pack_into('<III', e, 0, 0, m['uid'], 0x03650011)
        e[0x24:0x44] = cstr(m['name'], 32)
        struct.pack_into('<III', e, 0x44, 2, 0x11223344, len(m['segs']))
        for attr, va, size in m['segs']:
            e += struct.pack('<5I', 0, attr, va, size, 0x10)
        e += b'\0' * 0x10
        out += bytes(e)
    return out


def app_info(title, name):
    d = bytearray(260)
    struct.pack_into('<II', d, 0, 6, 0)
    d[0x08:0x12] = cstr(title, 10)
    d[0x12:0x92] = cstr(name, 128)
    return bytes(d)


def system_info(fw=0x03650011):
    d = bytearray(96)
    struct.pack_into('<IIIII', d, 0, 4, 0, fw, 0, fw)
    return bytes(d)


def process_info(title):
    d = bytearray(148)
    struct.pack_into('<II', d, 0, 1, 0)
    d[0x10:0x20] = cstr(title, 16)
    return bytes(d)


def common_modules():
    return [
        {'uid': 0x4001000b, 'name': 'SceLibKernel', 'segs': [(5, 0xe0000390, 0xe8d8), (6, 0xe0011d60, 0x48)]},
        {'uid': 0x40010013, 'name': 'SceGxm', 'segs': [(5, 0xe006c100, 0x23d04), (6, 0xe001ac20, 0x72c)]},
        {'uid': 0x40010001, 'name': 'd2vita_boot.elf', 'segs': [(5, EBOOT_TEXT, 0x349000), (6, 0x81400000, 0x387430)]},
    ]


def thr(uid, name, status, stop, pc, lr=0, sp=0x81c003b8, r=None, cpsr=0x200f0010):
    regs = list(r) if r else [0] * 13
    if len(regs) < 13:
        regs += [0] * (13 - len(regs))
    return {'uid': uid, 'name': name, 'status': status, 'stop': stop, 'pc': pc, 'lr': lr, 'sp': sp, 'r': regs, 'cpsr': cpsr}


def guest_regs(esp, ebp):
    r = [0] * 13
    r[8], r[9] = esp, ebp      # box86 map: xESP=r8, xEBP=r9 (tools/autopsie_psp2dmp.py:21)
    return r


def stack_segment(guest_lo, size, frames, extra=()):
    """Guest stack [guest_lo, guest_lo+size) seen through the host view,
    with chained EBP frames: frames = [(ebp, next_ebp, ret), ...]."""
    buf = bytearray(size)
    for i in range(0, size, 4):
        struct.pack_into('<I', buf, i, 0xdeadbeef ^ (i * 2654435761 & 0xffffffff))
    for ebp, nxt, ret in frames:
        struct.pack_into('<II', buf, ebp - guest_lo, nxt, ret)
    for off, data in extra:
        buf[off:off + len(data)] = data
    return (HOST + guest_lo, bytes(buf))


def jit_segment(va, size):
    buf = bytearray()
    while len(buf) < size:
        buf += struct.pack('<I', 0xe1a00000)       # ARM nop (mov r0, r0)
    return (va, bytes(buf[:size]))


def filler(va, size, seed):
    buf = bytearray(size)
    x = seed
    for i in range(0, size, 4):
        x = (x * 1103515245 + 12345) & 0xffffffff
        struct.pack_into('<I', buf, i, x if (i // 4) % 16 == 0 else 0)
    return (va, bytes(buf))


def build(notes, loads, truncate_to=None, elf_only=False, trailing=()):
    """notes: note blobs before the memory; loads: (va, bytes[, claimed_size]);
    trailing: note blobs after the memory, as in real dumps."""
    phnum = len(notes) + len(loads) + len(trailing)
    off = 52 + 32 * phnum
    ph = b''
    body = b''
    for n in notes:
        ph += struct.pack('<8I', 4, off, 0, 0, len(n), 0, 0, 4)
        body += n
        off += len(n)
    for l in loads:
        va, data = l[0], l[1]
        claimed = l[2] if len(l) > 2 else len(data)
        ph += struct.pack('<8I', 1, off, va, 0, claimed, claimed, 7, 4)
        body += data
        off += claimed
    for n in trailing:
        ph += struct.pack('<8I', 4, off, 0, 0, len(n), 0, 0, 4)
        body += n
        off += len(n)
    ident = b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\0' * 8
    hdr = ident + struct.pack('<HHIIIIIHHHHHH', 4, 40, 1, 0, 52, 0, 0x05000000, 52, 32, phnum, 0, 0, 0)
    elf = hdr + ph + body
    if truncate_to is not None:
        elf = elf[:truncate_to]
    return elf if elf_only else gzip.compress(elf, 6)


def trailing_notes(big=0x60000):
    # Second note group of real dumps (types observed: MEM_BLK_INFO 4103,
    # TTY_INFO 4138, SYSTEM_INFO2 4141 of 4 MiB, SUMMARY_INFO 4142); contents
    # here are placeholders. SYSTEM_INFO2 is larger than the reader's note cap.
    return [note('MEM_BLK_INFO', b'\0' * 64, 4103), note('TTY_INFO', b'boot tty output\n'.ljust(256, b'\0'), 4138),
            note('SYSTEM_INFO2', filler(0, big, 77)[1], 4141), note('SUMMARY_INFO', b'\0' * 32, 4142)]


def stamp_note():
    text = ('D2VSTAMP1 session_id=%s build_id=%s\n' % (SESSION_ID, BUILD_ID)).encode()
    return note('D2VITA_STAMP', text, 0x7000)


def dump_first_jit(elf_only=False):
    # Main thread first and faulting inside translated code, like the D2Vita
    # dumps; 5 guest frames, then a return address outside Game.exe ends the chain.
    esp, ebp = 0x0f2e1000, 0x0f2e1028
    frames = [(0x0f2e1028, 0x0f2e1100, GAME + 0x51c23), (0x0f2e1100, 0x0f2e1400, GAME + 0x4f570),
              (0x0f2e1400, 0x0f2e2000, GAME + 0x4b959), (0x0f2e2000, 0x0f2e4000, GAME + 0x4b43c),
              (0x0f2e4000, 0x0f2e8000, GAME + 0x5e6b), (0x0f2e8000, 0x0f2ec000, 0x01d00010)]
    threads = [thr(0x40010003, 'DTWO00001', 1, 0x30004, JIT + 0x24184, lr=JIT + 0x24101, r=guest_regs(esp, ebp)),
               thr(0x4001011b, 'd2_present', 8, 0, 0xe0006694, lr=0xe000ace7, sp=0x813e0f58),
               thr(0x4002017b, 'SceGxmDisplayQueue', 8, 0, 0xe0006aa4, lr=0xe000ac75, sp=0x813e3fa8)]
    notes = [note('COREFILE_INFO', b'\0' * 40, NT_COREFILE), note('THREAD_INFO', thread_info(threads), NT_THREAD),
             note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG), note('PROCESS_INFO', process_info('DTWO00001'), NT_PROCESS),
             note('SYSTEM_INFO', system_info(), NT_SYSTEM), note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP),
             note('MODULE_INFO', module_info(common_modules()), NT_MODULE), stamp_note()]
    loads = [filler(0x81801000, 0x8000, 7), stack_segment(0x0f2e0000, 0x20000, frames), jit_segment(JIT + 0x24000, 0x1000)]
    return build(notes, loads, elf_only=elf_only, trailing=trailing_notes())


def dump_worker_withheld():
    # Faulting runner listed third (as in real non-D2 dumps): the first thread
    # is NOT the faulting one. Fake secrets are planted, one across a 64 KiB
    # boundary of the decompressed stream. Chain: 3 frames, then a frame
    # pointer outside every PT_LOAD.
    esp, ebp = 0x0f4e1000, 0x0f4e1010
    frames = [(0x0f4e1010, 0x0f4e1200, GAME + 0x1fecc5), (0x0f4e1200, 0x0f4e1800, GAME + 0x2003a7),
              (0x0f4e1800, 0x0f4f0010, GAME + 0xdbda8)]
    threads = [thr(0x40010003, 'DTWO00001', 8, 0, EBOOT_TEXT + 0x305258, lr=EBOOT_TEXT + 0x9bba84, r=[0] * 9 + [0xffffffff]),
               thr(0x4001011b, 'd2_present', 8, 0, 0xe0006694, lr=0xe000ace7, sp=0x813e0f58),
               thr(0x40020301, 'd2_runner3', 1, 0x30004, JIT + 0x300a0, lr=JIT + 0x30001, sp=0x81a00f00, r=guest_regs(esp, ebp))]
    key_ascii = b'FAKE-1234-CLAS-5678'
    key_utf16 = 'fakelodkey0123456789abcdef'.encode('utf-16-le')
    notes = [note('COREFILE_INFO', b'\0' * 40, NT_COREFILE), note('THREAD_INFO', thread_info(threads), NT_THREAD),
             note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG), note('SYSTEM_INFO', system_info(), NT_SYSTEM),
             note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP), note('MODULE_INFO', module_info(common_modules()), NT_MODULE)]
    stack = stack_segment(0x0f4e0000, 0x10000, frames, extra=[(0x3000, key_ascii)])
    heap = bytearray(filler(0x85123000, 0x30000, 11)[1])
    heap[0x100:0x100 + len(key_utf16)] = key_utf16
    loads = [stack, (0x85123000, bytes(heap)), jit_segment(JIT + 0x30000, 0x1000)]
    # Place "FAKEACCOUNT" across the 0x40000 (256 KiB) decompressed offset,
    # i.e. across a 64 KiB output chunk boundary.
    elf = bytearray(build(notes, loads, elf_only=True))
    word = b'FAKEACCOUNT'
    at = 0x40000 - 5
    elf[at:at + len(word)] = word
    return gzip.compress(bytes(elf), 6)


def dump_eboot_nostamp():
    threads = [thr(0x40010003, 'DTWO00001', 1, 0x30004, EBOOT_TEXT + 0x2f6e6e, lr=EBOOT_TEXT + 0x2fa1f1, r=[0x9f, 0x98, 1, 0x53, 7, 0x9f, 0, 0, 1, 0xffffffff, 0, 0, 0xffffffff], cpsr=0x200f0030),
               thr(0x4001011b, 'd2_present', 8, 0, 0xe0006694, lr=0xe000ace7, sp=0x813e0f58)]
    notes = [note('THREAD_INFO', thread_info(threads), NT_THREAD), note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG),
             note('SYSTEM_INFO', system_info(), NT_SYSTEM), note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP),
             note('MODULE_INFO', module_info(common_modules()), NT_MODULE)]
    loads = [filler(0x81c00000, 0x1000, 3), (EBOOT_TEXT + 0x2f6e4e - 0x4e, b'\0' * 0x100)]
    return build(notes, loads)


def dump_other_app_sysmodule():
    threads = [thr(0x40010003, 'CARN20001', 1, 0x30002, 0xe0006694, lr=0xe000ace7, sp=0x82100018, cpsr=0x800f0030)]
    mods = [{'uid': 0x4001000b, 'name': 'SceLibKernel', 'segs': [(5, 0xe0000390, 0xe8d8)]},
            {'uid': 0x40010001, 'name': 'carnvita.elf', 'segs': [(5, EBOOT_TEXT, 0x36ddb8)]}]
    notes = [note('THREAD_INFO', thread_info(threads), NT_THREAD), note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG),
             note('SYSTEM_INFO', system_info(), NT_SYSTEM), note('APP_INFO', app_info('CARN20001', 'Carnivores 2'), NT_APP),
             note('MODULE_INFO', module_info(mods), NT_MODULE)]
    return build(notes, [filler(0x82100000, 0x1000, 5)])


def dump_d2_sysmodule():
    # D2Vita dump (no stamp) whose PC is inside a system module, and whose LR
    # is inside a module named "Sce Odd-Module": a name the contract cannot
    # carry (space, dash), so that address must come out as region unknown.
    esp, ebp = 0x0f2e1000, 0x0f2e1028
    frames = [(0x0f2e1028, 0x0f2e1100, GAME + 0x51c23), (0x0f2e1100, 0x0f2f0000, 0x01d00010)]
    threads = [thr(0x40010003, 'DTWO00001', 1, 0x30004, 0xe0006694, lr=0xe1004321, r=guest_regs(esp, ebp))]
    mods = common_modules() + [{'uid': 0x40010045, 'name': 'Sce Odd-Module', 'segs': [(5, 0xe1000000, 0x10000)]}]
    notes = [note('THREAD_INFO', thread_info(threads), NT_THREAD), note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG),
             note('SYSTEM_INFO', system_info(), NT_SYSTEM), note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP),
             note('MODULE_INFO', module_info(mods), NT_MODULE)]
    return build(notes, [stack_segment(0x0f2e0000, 0x10000, frames)])


def eboot_thread():
    return [thr(0x40010003, 'DTWO00001', 1, 0x30004, EBOOT_TEXT + 0x2f6e6e, lr=EBOOT_TEXT + 0x2fa1f1),
            thr(0x4001011b, 'd2_present', 8, 0, 0xe0006694, lr=0xe000ace7, sp=0x813e0f58)]


def first_group(threads, extra=()):
    return [note('THREAD_INFO', thread_info(threads), NT_THREAD), note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG),
            note('SYSTEM_INFO', system_info(), NT_SYSTEM), note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP),
            note('MODULE_INFO', module_info(common_modules()), NT_MODULE)] + list(extra)


def stale_memory():
    # Stamp look-alikes in memory: the stamp format string of the eboot's
    # rodata, and a well-formed stamp of another session left in the heap.
    rodata = bytearray(0x1000)
    fmt = b'D2VSTAMP1 session_id=%s build_id=%s\n\0'
    rodata[0x100:0x100 + len(fmt)] = fmt
    heap = bytearray(0x1000)
    stale = ('D2VSTAMP1 session_id=%s build_id=%s\n' % ('f' * 32, BUILD_ID)).encode()
    heap[0x200:0x200 + len(stale)] = stale
    return [(0x81400000, bytes(rodata)), (0x85100000, bytes(heap))]


def dump_stamp_lookalikes():
    # No stamp note: nothing in memory may pass for one.
    return build(first_group(eboot_thread()), stale_memory())


def dump_stamp_malformed_note():
    # A stamp note whose session id is not 32 lowercase hex digits.
    bad = note('D2VITA_STAMP', b'D2VSTAMP1 session_id=0123456789ABCDEF build_id=0.1.0+ab12cd34ef56\n', 0x7000)
    return build(first_group(eboot_thread(), [bad]), [filler(0x81c00000, 0x1000, 3)])


def dump_stamp_trailing():
    # The stamp note sits in the second note group, after the memory (more
    # than one 64 KiB inflate chunk of it, as in real dumps), and a foreign
    # stamp lies in the memory before it.
    loads = stale_memory() + [filler(0x86000000, 0x30000, 21)]
    return build(first_group(eboot_thread()), loads, trailing=trailing_notes(0x1000) + [stamp_note()])


def dump_truncated():
    # PT_LOAD headers claim more bytes than the file holds (seen in two real
    # dumps whose gzip stream is nevertheless complete).
    esp, ebp = 0x0f2e1000, 0x0f2e1028
    frames = [(0x0f2e1028, 0x0f2e1100, GAME + 0x100), (0x0f2e1100, 0x0f2e1100, GAME + 0x200)]
    threads = [thr(0x40010003, 'DTWO00001', 1, 0x30004, JIT + 0x24184, lr=JIT + 0x24101, r=guest_regs(esp, ebp))]
    notes = [note('THREAD_INFO', thread_info(threads), NT_THREAD), note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG),
             note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP)]
    loads = [stack_segment(0x0f2e0000, 0x10000, frames), jit_segment(JIT + 0x24000, 0x1000),
             (0x81801000, b'\0' * 0x1000, 0x400000)]
    # The trailing notes are announced but the file ends inside the last
    # PT_LOAD, as in two real dumps.
    late = trailing_notes(0x1000)
    elf = build(notes, loads, elf_only=True, trailing=late)
    elf = elf[:len(elf) - sum(len(n) for n in late)]
    return gzip.compress(elf, 6)          # the ELF itself is shorter than its phdrs say


def dump_long_chain():
    esp, ebp = 0x0f2e1000, 0x0f2e1000
    frames = []
    e = ebp
    for i in range(30):
        nxt = e + 0x200
        frames.append((e, nxt, GAME + 0x1000 + i))
        e = nxt
    threads = [thr(0x40010003, 'DTWO00001', 1, 0x30004, JIT + 0x24184, lr=JIT + 0x24101, r=guest_regs(esp, ebp))]
    notes = [note('THREAD_INFO', thread_info(threads), NT_THREAD), note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG),
             note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP)]
    return build(notes, [stack_segment(0x0f2e0000, 0x10000, frames)])


def dump_big():
    # About 9 MiB decompressed, like the largest real dumps: two copies of a
    # 4 MiB main stack at the same address (also seen in real dumps).
    esp, ebp = 0x0f2e1000, 0x0f2e1028
    frames = [(0x0f2e1028, 0x0f2e1100, GAME + 0x51c23), (0x0f2e1100, 0x0f2f0f00, GAME + 0x4f570),
              (0x0f2f0f00, 0x0f2f1000, GAME + 0x12345)]
    threads = [thr(0x40010003, 'DTWO00001', 1, 0x30004, JIT + 0x24184, lr=JIT + 0x24101, r=guest_regs(esp, ebp))]
    notes = [note('THREAD_INFO', thread_info(threads), NT_THREAD), note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG),
             note('SYSTEM_INFO', system_info(), NT_SYSTEM), note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP),
             note('MODULE_INFO', module_info(common_modules()), NT_MODULE), stamp_note()]
    main_stack = filler(0x81801000, 0x400000, 99)
    loads = [main_stack, main_stack, stack_segment(0x0f2e0000, 0x20000, frames), jit_segment(JIT + 0x24000, 0x1000),
             filler(0xd0001000, 0x10000, 42)]
    return build(notes, loads, trailing=trailing_notes(0x400000))


def dump_late_threads():
    # Hypothetical layout: thread notes only in the second note group. The
    # reader still names the thread, but memory has already streamed by.
    esp, ebp = 0x0f2e1000, 0x0f2e1028
    frames = [(0x0f2e1028, 0x0f2e1100, GAME + 0x51c23)]
    threads = [thr(0x40010003, 'DTWO00001', 1, 0x30004, JIT + 0x24184, lr=JIT + 0x24101, r=guest_regs(esp, ebp))]
    notes = [note('APP_INFO', app_info('DTWO00001', 'D2Vita Boot'), NT_APP)]
    late = [note('THREAD_INFO', thread_info(threads), NT_THREAD), note('THREAD_REG_INFO', thread_reg_info(threads), NT_THREAD_REG)]
    return build(notes, [stack_segment(0x0f2e0000, 0x10000, frames)], trailing=late)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    first_jit = dump_first_jit()
    files = {
        'first_jit.psp2dmp': first_jit,
        'raw_elf.psp2dmp': dump_first_jit(elf_only=True),
        'worker_withheld.psp2dmp': dump_worker_withheld(),
        'eboot_nostamp.psp2dmp': dump_eboot_nostamp(),
        'other_app_sysmodule.psp2dmp': dump_other_app_sysmodule(),
        'd2_sysmodule.psp2dmp': dump_d2_sysmodule(),
        'stamp_lookalikes.psp2dmp': dump_stamp_lookalikes(),
        'stamp_malformed_note.psp2dmp': dump_stamp_malformed_note(),
        'stamp_trailing.psp2dmp': dump_stamp_trailing(),
        'truncated.psp2dmp': dump_truncated(),
        'long_chain.psp2dmp': dump_long_chain(),
        'big.psp2dmp': dump_big(),
        'late_threads.psp2dmp': dump_late_threads(),
        'gzip_cut.psp2dmp': first_jit[:len(first_jit) * 6 // 10],
        'not_elf.psp2dmp': gzip.compress(b'this is not an ELF core dump' * 10),
    }
    for name, data in files.items():
        with open(os.path.join(a.out, name), 'wb') as f:
            f.write(data)


if __name__ == '__main__':
    main()
