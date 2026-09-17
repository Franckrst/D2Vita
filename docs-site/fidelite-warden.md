# Fidelity Warden / anti-cheat

Technical backlog referenced from [Online play](en-ligne.md#what-remains-open):
the precise inventory of what distinguishes this runtime from a real Windows
process, and why each gap was left as-is rather than hidden or worked around.
The player-facing summary is on the [Online play](en-ligne.md) page.

---

Prioritized backlog from the runtime-fidelity work. Ground rules, unchanged:
**no bypass, no synthetic responses, no anti-cheat special-casing; every item
must also benefit ordinary Diablo; expose the real guest state, never a
fabricated view.**

These are queued behind day-to-day play. Do NOT let further fidelity work
delay ordinary gameplay work unless a given item directly explains a real
bug.

## Already fixed (Windows-identity fidelity)

Each of these had a test proving the gap BEFORE and its absence AFTER
(`D2FIDTEST`):

- `GetCurrentProcessId` used to return `1` — no real Windows PID is ever `1`,
  and they're allocated in steps of 4. Returns a plausible PID now; same fix
  for `th32ProcessID` (Toolhelp/Module32).
- Guest-visible thread ids used to be sequential (1, 2, 3…) instead of
  Windows-shaped values, with conversion in both directions (`OpenThread`).
  Internal ids (logs, benches, profiles) are unaffected — only the guest's
  own view changed.
- `IsProcessorFeaturePresent` used to return 0 for everything while the
  emulated `cpuid` advertised SSE/SSE2 — an internal contradiction. Answers
  are now derived from those same bits.
- `GetModuleFileName(hKernel32)` used to return `C:\Diablo II\game.exe`. An
  unknown handle now fails honestly (`ERROR_INVALID_HANDLE`) instead of
  fabricating a system path (the broader "no system modules present" gap
  remains open, tracked below).
- `sendto`/`recvfrom` were unshimmed, causing a controlled stop on the
  official UDP handshake. Implemented.
- The CD key used to live inside `registry.txt` (swept up by ordinary log
  collection). Moved to a dedicated keystore outside the normal write root.
- `connect()` had no safety net. Official-server traffic is now allowed by
  default (a public release meant to be played online), with one
  unconditional restriction: refused outright while any debugging/
  automation tool is active. `D2_LOCAL_ONLY=1` opts back into the old
  restrictive behavior (local allowlist only) for development — the
  generic mechanism lives in `winx86`, the policy here.
- **A guest image that is never written to.** There is no longer a
  non-pristine path: the last `.text` patch (the native blit's 5-byte `E9`)
  became a translation-time redirect, the IAT-stub optimization is gone, and
  the `D2_PRISTINE` flag with them. `D2_PRISTINE_CHECK` verifies the
  invariant; only the explicitly-armed `D2_NOCAP` bench knob still writes
  game bytes, and it must never be set for online play.
- **Real Authenticode** (`WIN_CERTIFICATE` + PKCS#7 + publisher blob,
  `third_party/winx86/src/runtime/authenticode.cpp`) — real chain-of-trust
  verification, no fabricated response; matches real Windows
  (`Get-AuthenticodeSignature` on the same binary also reports `Valid`).

## Deferred, with the reason

- [ ] `QueryPerformanceFrequency` = 1,000,000. Wrong relative to a real PC,
      but consistent with this runtime's own QPC (microseconds). Changing it
      alone creates a new contradiction; changing it together with RDTSC
      shifts guest timing — real risk to both the image oracle and
      performance. To be done once, on a dedicated bench, never in passing.
- [ ] `OpenProcess(self)` succeeding, PEB/`PEB_LDR_DATA`, real per-section
      protection tracking (and `VirtualProtect` returning the real previous
      protection), the trap window and MISC stubs excluded from
      `VirtualQuery`, scheduler stacks/TIBs reported as `MEM_FREE`.

## Known gap — no structured exception handling

A guest fault kills the faulting thread. The game's own `__try/__except` never
runs, `RtlUnwind` does nothing, and the top-level filter registered through
`SetUnhandledExceptionFilter` is recorded but never invoked. Storm guards some
file and registry parsing with SEH, so a case a real Windows would have
absorbed ends the process here instead.

An earlier dispatcher was removed rather than finished: it was written for the
cooperative scheduler and cannot work under the native one (calling a guest
handler from inside a shim already holding the GIL self-deadlocks), so a real
implementation is a rewrite, not a continuation. What survives is the honest
part — on a fault the runtime records whether a chain was installed
(`fault_note_seh_chain`), so a crash report says "the game would have caught
this" instead of leaving it unexplained.

It requires, all together: a `%fs:0` model that agrees with the guest's own
view, a correct `EXCEPTION_REGISTRATION_RECORD` walk, handler return
semantics, resume, `RtlUnwind`, and a real `__try/__except` test (mingw GCC
i686 emits no MSVC SEH, so that test needs a hand-crafted frame).

**Do not start it on the strength of the argument above — the prerequisite is
elsewhere.** A dedicated study (static PE scans and disassembly, no run on
hardware) established that in the shipped configuration a guest access
violation never becomes an observable fault at all: `D2ARENA` + `D2LAYOUT=compact`
are forced in `src/platform/d2_boot_config.cpp`, and the translated code's
address translation is a single unchecked add (`MMU_G2H`), which the engine's
own comment describes as never faulting. A wild guest pointer therefore reads
or writes arena memory silently — a null dereference lands in the main
thread's TIB — instead of raising anything an SEH dispatcher could deliver.
Under the qemu gate the memory model is different (identity mmap, so a wild
pointer really does fault), which means **the gate does not exercise the
console's behaviour here**. Caveat carried from the study: the Vita arena
(`sceKernelAllocMemBlock`) was reasoned about from the `mmap` path, not
observed on hardware.

So the honest statement of the debt is not "SEH is missing". It is: *in arena
mode a guest access violation is undetectable; SEH fidelity is the consumer of
that detection, not its prerequisite.* Whoever picks this up starts with bounds
checking in the translated memory accesses — on a frame budget that fights for
single-digit percentages — and only then asks the SEH question again, with a
measured price. A dispatcher that resumes on a half-correct `CONTEXT`, or a
no-op `RtlUnwind` that leaves `fs:[0]` pointing at dead frames, continues the
game with a wrong state and surfaces far from its cause: worse than the crash
it replaces.

One measurement worth keeping: a fault taken while the guest holds a critical
section frees nothing (`WxCrit` records an owner and the thread dies without
releasing it), so the symptom is other threads hanging, not a crash. Any freeze
investigation should rule this out before concluding elsewhere.

## P1 — VirtualProtect

- [ ] Track protections per region/page (today the arena is effectively RWX and
      `lpflOldProtect` is always reported as `0x40`).
- [ ] Return the real previous `lpflOldProtect`.
- [ ] Test RW→RX→RW and partial-range protections.

## P2 — Self-process introspection

`OpenProcess` must not stay artificially denied forever. Implement — no urgency,
no anti-cheat hack — a real handle for our own guest process with coherent
Win32 semantics, reading the real guest memory, with **no fake content and no
Warden-specific path**:

- [ ] `OpenProcess(self)` → a real process handle.
- [ ] `ReadProcessMemory` already reads real guest memory, but only accepts the
      current-process pseudo-handle (`-1`) — it must accept the real handle
      above, which is why the two are done together.
- [ ] `VirtualQueryEx(self, …)` → same region model as `VirtualQuery`.

## P3 — Guest loader (medium term)

Only when the above are done and if a real path needs it. Do **not** synthesize
fictitious system DLLs just to satisfy a check.

- [ ] Minimal PEB.
- [ ] `PEB_LDR_DATA`.
- [ ] `LDR_DATA_TABLE_ENTRY` for the real guest PE modules.
- [ ] Coherence with `GetModuleHandle` / Toolhelp / `VirtualQuery`.

---

## Already done (pass 1 + pass 2, on `main`)

UNSHIMMED controlled-stop; guest-pristine EXE at 0x00400000 zero-reloc; dynamic-code
invalidation (VirtualProtect/FlushInstructionCache) + dedicated D2FIDTEST cycle
(13/13); honest VirtualQuery region model; honest Toolhelp/Thread32/Process32/
OpenThread/GetThreadContext; canonical CONTEXT offsets (RtlCaptureContext/
GetThreadContext); GetModuleHandleEx by-name + GetProcAddress miss codes; semaphore
max clamp; SEH classification + fail-safe dispatcher foundation; the generic
Windows→Wine→D2Vita torture-test (`tools/torture/`, gated `D2_RUNEXE`).
