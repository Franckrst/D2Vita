#!/usr/bin/env bash
# tools/build_rt_boot_vpk_fast.sh — builds the full rt_boot runtime as a PS Vita VPK.
# ORCHESTRATION variant of tools/build_rt_boot_vpk.sh: same sources, same
# flags, same object order at link time, but
#   (1) the 13 units are compiled IN PARALLEL (default: nproc-1),
#   (2) a unit is only recompiled if it changed — including header
#       dependencies, via the .d files produced by -MMD -MP,
#   (3) the tail (link, nm guard, velf, fself, sfo, vpk) is skipped when no
#       object has moved AND every product is up to date.
# No COMPILE flag is changed: -MMD -MP only emit the .d files and are not
# part of CXXFLAGS (so not part of the link line either).
#
# Settings:
#   D2VPK_REBUILD=1   or   --rebuild   : full build, ignores the cache
#   D2VPK_JOBS=N                       : number of simultaneous compiles
#   D2VPK_OUT=/path                    : output directory (default build-vita)
#   D2VPK_PROF=1                       : build WITH -DPROF_COUNTERS (default: WITHOUT)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
OUT="${D2VPK_OUT:-$ROOT/build-vita}"

# Dependency tracking splits .d files on whitespace: a path containing a
# space would break detection SILENTLY (= a binary that no longer matches its
# source). This refuses to guess.
case "$ROOT$OUT" in
  *[[:space:]]*) echo "FATAL: chemin avec espace ($ROOT / $OUT) — suivi de dependances non fiable" >&2; exit 1;;
esac

FORCE_ALL="${D2VPK_REBUILD:-0}"
for a in "$@"; do
  case "$a" in
    --rebuild|-B) FORCE_ALL=1 ;;
    *) echo "argument inconnu: $a (attendu: --rebuild)" >&2; exit 2 ;;
  esac
done

# TITLE_ID MUST be exactly 9 chars (XXXX#####) — a 10-char id installs fine
# on Vita3K (lax) but real hardware rejects the VPK with error 0xF0030000.
TITLE="${TITLE:-DTWO00001}"
APPNAME="${APPNAME:-D2Vita}"
VPKOUT="${VPKOUT:-d2vita.vpk}"
EXTRA_DEFS=""
# D2VPK_SHACC=1: shader-BUILDING flavor. Links the console's Cg compiler
# (SceShaccCg / libshacccg.suprx) to produce the .gxp files once, into
# ux0:data/d2vita/shaders. This module is NOT present by default on a retail
# Vita: an unresolved import can get the whole eboot rejected, breaking the
# GDI path too. The SHIPPED VPK never links it — it reads the .gxp files instead.
SHACC_LD=""
if [ "${D2VPK_SHACC:-0}" = "1" ]; then
  # WEAK stub: an unresolved weak import doesn't get the module rejected,
  # where a strong stub would. This is the only acceptable form here, even
  # in a build-flavor.
  EXTRA_DEFS="$EXTRA_DEFS -DD2_SHACC"; SHACC_LD="-lSceShaccCg_stub_weak"
  echo "== D2VPK_SHACC=1 : saveur FABRICATION DE SHADERS (lie SceShaccCg) — ne pas livrer =="
fi
# D2VPK_TLSWRAP=1: counts REAL calls to __emutls_get_address (MEASUREMENT
# build, never shipped). Adds the define AND the link wrap together:
# splitting them would give a counter that's always zero, i.e. a silent
# diagnostic.
WRAP_LD=""
[ "${D2VPK_TLSWRAP:-0}" = "1" ] && { EXTRA_DEFS="$EXTRA_DEFS -DD2_TLSWRAP"; WRAP_LD="-Wl,--wrap=__emutls_get_address"; \
  echo "== D2VPK_TLSWRAP=1 : build de MESURE (compteur emutls) — NE PAS LIVRER =="; }
# Generic shim A/B flavor:
#   D2VPK_TAG=SANSBLIT D2VPK_OFF=NATIVEBLIT TITLE=DTWO00011 ...
# TAG names the log and save folder, OFF lists the knobs to force to 0.
if [ -n "${D2VPK_TAG:-}" ]; then
  EXTRA_DEFS="$EXTRA_DEFS -DD2VPK_TAG=\"$D2VPK_TAG\" -DD2VPK_OFF=\"${D2VPK_OFF:-}\""
fi
# D2VPK_PROF: -DPROF_COUNTERS. Default = WITHOUT. The instrument stays one
# knob away: D2VPK_PROF=1 restores the instrumented build.
#
# WHY the default is off: Bridge::trap_handler calls prof::now_ns() TWICE per
# trap, and on Vita now_ns() = sceKernelGetProcessTimeWide(), an INTER-MODULE
# call (src/runtime/prof.cpp). Measured on hardware, those two calls cost
# ~4.4% of frame time — well above the bench's noise floor. In other words,
# the shipped build was paying for instrumentation nobody reads in play. The
# per-trap residual WITHOUT the flag is zero by construction (prof.h provides
# empty inlines).
#
# This default is backed by hardware measurement, not a guess: the no-PROF
# build was run repeatedly on real hardware with zero regression.
PROF_DEF=""
if [ "${D2VPK_PROF:-0}" = "1" ]; then
  PROF_DEF="-DPROF_COUNTERS"
  echo "== D2VPK_PROF=1 : build AVEC -DPROF_COUNTERS (instrument, +4,4 % mesure) =="
fi

mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# Crash-report build_id (spec 4.10): "<VERSION>+<12 hex of HEAD>
# [-dirty]", injected into src/crashreport/build_id.cpp below via -D so it
# lives in that ONE unit's command-line fingerprint (need_rebuild()) rather
# than in a header every unit would depend on — a commit alone (no file
# touched) still forces exactly that unit to recompile, with no separate
# "always recompiled" mechanism needed. Recomputed EXACTLY the way
# tools/crash/d2vcrash/cli.py:local_build_id() does, so a console report and
# `crash builds register` (further below) name the same build the same way.
# ---------------------------------------------------------------------------
VERSION_FILE="$ROOT/VERSION"
[ -f "$VERSION_FILE" ] || { echo "FATAL: pas de fichier VERSION a la racine du depot ($VERSION_FILE)" >&2; exit 1; }
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
GIT_COMMIT="$(git -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null || true)"
[ -n "$GIT_COMMIT" ] || { echo "FATAL: commit git introuvable pour $ROOT (build_id en a besoin)" >&2; exit 1; }
GIT_DIRTY=""
[ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ] && GIT_DIRTY="-dirty"
BUILD_ID="${VERSION}+${GIT_COMMIT}${GIT_DIRTY}"
# release ONLY for the maintainer's own explicit publish step
# (D2VPK_CHANNEL=release) — every other build (local, CI, a fresh clone) is
# dev by default: dev/test builds get the relaxed rate-limit tier of
# contract/README.md section 5.5, so defaulting to release would risk an
# ordinary build quietly eating the shared production quota.
CHANNEL="${D2VPK_CHANNEL:-dev}"
case "$CHANNEL" in
  release|dev|test) ;;
  *) echo "FATAL: D2VPK_CHANNEL=$CHANNEL inconnu (release|dev|test)" >&2; exit 1 ;;
esac
echo "== crash-report build_id=$BUILD_ID channel=$CHANNEL =="

# ---------------------------------------------------------------------------
# D2V_CRASHTEST=1: builds in the console-triggered synthetic-crash test paths
# (D2_CRASHTEST=native|halt at runtime — spec section 10 "Console",
# src/crashreport/cr_crashtest.h) used ONLY to validate the crash-report
# pipeline on real hardware without reproducing an organic bug. Absent (the
# default, and the only thing any normal/shipping build ever does), the
# define is never passed and none of that code exists in the binary at all.
#
# Safety against shipping this by accident, two layers:
#   1. loud banner below, impossible to miss in the build log;
#   2. hard refusal when combined with channel=release — release is ONLY the
#      maintainer's own explicit publish step (D2VPK_CHANNEL=release, see the
#      CHANNEL comment above); every other invocation (local, CI, a fresh
#      clone) is dev by default. Refusing outright (rather than silently
#      forcing channel=test) is the simplest option that is ALSO the safest:
#      it makes "a crashtest build with channel=release" categorically
#      impossible instead of merely surprising, and it changes nothing about
#      how CHANNEL itself is computed for every normal build.
CRASHTEST_DEF=""
if [ "${D2V_CRASHTEST:-0}" = "1" ]; then
  if [ "$CHANNEL" = "release" ]; then
    echo "FATAL: D2V_CRASHTEST=1 avec D2VPK_CHANNEL=release refuse — un build de test de crash ne doit JAMAIS etre le build publie (voir tools/build_rt_boot_vpk.sh)" >&2
    exit 1
  fi
  CRASHTEST_DEF="-DD2V_CRASHTEST"
  echo "=================================================================="
  echo "==  D2V_CRASHTEST=1 : BUILD DE TEST DE CRASH (native|halt)        =="
  echo "==  D2_CRASHTEST=native|halt declenche un vrai plantage.          =="
  echo "==  NE JAMAIS PUBLIER CE BUILD (channel actuel: $CHANNEL).        =="
  echo "=================================================================="
fi

# The dynarec and the generic runtime layer (PE32/Cpu/Bridge/schedulers/
# generic Vita audio-keyboard) live in the third_party/winx86 submodule — see
# third_party/winx86/README.md for the extension contract
# (Cpu::set_alternate + Bridge::register_shim/shim_trap). What remains here
# is D2-specific.
WX86="$ROOT/third_party/winx86"
# PROF_COUNTERS touches code that now lives INSIDE libwinx86 (prof.cpp,
# sched_*.cpp, bridge.cpp): it must be REBUILT in this flavor, with a
# distinct LIBTAG (otherwise the non-instrumented and instrumented flavors
# would overwrite each other under the same archive name).
WX86_LIBTAG=""
[ "${D2VPK_PROF:-0}" = "1" ] && WX86_LIBTAG="_prof"
LIBTAG="$WX86_LIBTAG" EXTRA="$PROF_DEF" TARGET=vita bash "$WX86/build.sh" >/dev/null
DYNLIB="$WX86/build-vita/libwinx86_vita${WX86_LIBTAG}.a"

CXX=arm-vita-eabi-g++
# EXTRA_DEFS (D2VPK_TAG/OFF/REF/EXPL/SHACC/TLSWRAP) is part of the .cmd
# fingerprint of every file compiled with it — but only files that include
# platform/vita_present.h actually read these defines (D2VITA_PROGRESS_PATH
# depends on them): win32_shims_*, vita_net.cpp, scomp_pkware.cpp,
# dcc_native.cpp, d2_intrin_114.cpp, scomp_audio.cpp, huff.cpp, adpcm.cpp and
# explode.c need NONE of them. CXXFLAGS_BASE serves those (fingerprint stable
# across flavors); CXXFLAGS (= BASE + EXTRA_DEFS) serves the 8 units that
# include vita_present.h — the NEEDS_TAG list below. Splitting the two avoids
# invalidating all 19 units just for changing flavor on the 8 that care.
CXXFLAGS_BASE="-std=gnu++17 -O2 -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
  -D__vita__ -DD2RT_CPU_BOX86 $PROF_DEF $CRASHTEST_DEF \
  -I$ROOT/src -I$WX86/src -I$ROOT/src/glide_ring -I$WX86/src/dynarec86 -I$WX86/src/dynarec86/shim/vita \
  -I$WX86/src/dynarec86/shim -I$WX86/third_party/box86-dynarec/include \
  -I$WX86/third_party/box86-dynarec -I$ROOT/third_party/pklib -I$ROOT/third_party/monocypher \
  -Wno-unused-variable -Wno-unused-parameter"
CXXFLAGS="$CXXFLAGS_BASE $EXTRA_DEFS"
# Files whose object actually depends on EXTRA_DEFS (they include
# platform/vita_present.h, directly or via a header from the same module).
# cr_boot.cpp reads D2VITA_PROGRESS_PATH, which that header defines
# from D2VPK_TAG/REF/EXPL — it belongs in this list for exactly the same
# reason vita_present.cpp/d2_boot_config.cpp do.
NEEDS_TAG=" rt_boot.cpp replay60.cpp gx_host.cpp native_hooks_codec.cpp native_hooks_cellengine.cpp vita_present.cpp d2_boot_config.cpp vita_gxm.cpp lazy_seek.cpp io_stat.cpp scripted_input.cpp d2ini.cpp pristine_audit.cpp cell_frame_diag.cpp jit_profile.cpp frame_profile.cpp emutls_probe.cpp eip_time_profile.cpp tier1_intrinsics_install.cpp kernel32_files.cpp kernel32_modules.cpp kernel32_time.cpp win32_import_remainder.cpp checkrevision_crypto.cpp netguard_lock.cpp cr_boot.cpp "

# shellcheck source=tools/rt_boot_srcs.sh
source "$ROOT/tools/rt_boot_srcs.sh"
# shellcheck source=tools/crashreport_srcs.sh
source "$ROOT/tools/crashreport_srcs.sh"
SRCS=(
  "${RT_BOOT_SRCS_HEAD[@]}"
  "$ROOT/src/platform/vita_present.cpp"
  "$ROOT/src/platform/d2_boot_config.cpp"
  "$ROOT/src/platform/vita_gxm.cpp"
  "${RT_BOOT_SRCS_TAIL[@]}"
  "${CRASHREPORT_SRCS[@]}"
  "${CRASHREPORT_VITA_SRCS[@]}"
)

# pklib's C compile line, and the crash reporter's two Monocypher units
# (spec §8; cr_monocypher.c, NOT third_party/monocypher/monocypher.c — see
# tools/crashreport_srcs.sh for why). Each gets its OWN flags variable
# (looked up the same way TODO_FLAGSVAR already does for C++ units below):
# a single shared CFLAGS_C would force every C unit to the same include
# path, and monocypher-ed25519.c needs third_party/monocypher on its
# `#include` path where explode.c must not.
CC_C=arm-vita-eabi-gcc
CFLAGS_C="-O2 -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -std=gnu17 -I$ROOT/third_party/pklib"
SRC_C="$ROOT/third_party/pklib/explode.c"
CFLAGS_MONO="-O2 -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -std=gnu17"
CFLAGS_MONO_ED="-O2 -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -std=gnu17 -I$ROOT/third_party/monocypher"

# -MMD -MP: only to EMIT the .d files. Never in CXXFLAGS, so never on the
# link line either.
DEPFLAGS="-MMD -MP"

# Compiler version, folded into the .cmd fingerprint. -MMD only tracks
# project headers: a VitaSDK update wouldn't touch any source file's mtime,
# so stale objects would get relinked as-is. Comparing the compiler banner
# closes that gap for the common case (gcc changing version).
CXX_BANNER="$($CXX --version | head -1) | $($CC_C --version | head -1)"

# ---------------------------------------------------------------------------
# Object order: SRCS, then explode + the crash reporter's C units + its
# build_id.cpp, kept STRICT and stable.
# ---------------------------------------------------------------------------
EXTRA_C_SRCS=("$SRC_C" "$ROOT/src/crashreport/cr_monocypher.c" "$ROOT/third_party/monocypher/monocypher-ed25519.c")
EXTRA_C_FLAGSVARS=("CFLAGS_C" "CFLAGS_MONO" "CFLAGS_MONO_ED")
BUILDID_SRC="$ROOT/src/crashreport/build_id.cpp"
CXXFLAGS_BUILDID="$CXXFLAGS_BASE -DD2CR_BUILD_ID_STR=\"$BUILD_ID\" -DD2CR_CHANNEL_STR=\"$CHANNEL\""

OBJS=()
for s in "${SRCS[@]}"; do OBJS+=("$OUT/obj-$(basename "${s%.*}").o"); done
for s in "${EXTRA_C_SRCS[@]}"; do OBJS+=("$OUT/obj-$(basename "${s%.*}").o"); done
OBJS+=("$OUT/obj-$(basename "${BUILDID_SRC%.*}").o")

# ---------------------------------------------------------------------------
# Rebuild decision, unit by unit.
#
# Three reasons to recompile:
#   a) the object is missing;
#   b) the COMMAND LINE changed (.cmd fingerprint). Essential here: a
#      D2VPK_TAG flavor shares the same directory AND the same object names
#      as a normal build. Without this fingerprint, switching flavors would
#      reuse the previous flavor's objects — a binary that lies.
#   c) a dependency is newer than the object. The dependency list comes from
#      gcc's .d file: it contains the .cpp AND every project header actually
#      included. A missing .d or a vanished dependency => recompile (never
#      the reverse).
# ---------------------------------------------------------------------------
need_rebuild() { # $1=obj  $2=cmd-string ; prints the reason, returns 0 if it needs recompiling
  local o="$1" cmd="$2" d="${1%.o}.d" c="${1%.o}.cmd" dep
  [ "$FORCE_ALL" = "1" ] && { echo "build complet demande"; return 0; }
  [ -f "$o" ] || { echo "objet absent"; return 0; }
  [ -f "$c" ] || { echo "empreinte de commande absente"; return 0; }
  [ "$(cat "$c")" = "$cmd" ] || { echo "drapeaux/commande/compilateur modifies"; return 0; }
  [ -f "$d" ] || { echo "fichier .d absent"; return 0; }
  # Splitting the .d file: discards the target ("xxx.o:"), the continuation
  # backslashes, and -MP's phony targets ("header.h:" lines).
  while read -r dep; do
    [ -n "$dep" ] || continue
    if [ ! -e "$dep" ]; then echo "dependance disparue: $dep"; return 0; fi
    if [ "$dep" -nt "$o" ]; then echo "plus recent: ${dep#$ROOT/}"; return 0; fi
  done < <(tr -s ' \\\t' '\n\n\n' < "$d" | sed -e '/:$/d' -e '/^$/d')
  return 1
}

JOBS="${D2VPK_JOBS:-$(( $(nproc) - 1 ))}"
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1

echo "== compiling rt_boot runtime for Vita (parallele -j$JOBS, incremental) =="
[ "$FORCE_ALL" = "1" ] && echo "   (D2VPK_REBUILD/--rebuild : cache ignore)" || true

TODO_SRC=(); TODO_OBJ=(); TODO_CMD=(); TODO_WHY=(); TODO_KIND=(); TODO_FLAGSVAR=(); SKIPPED=()
for i in "${!SRCS[@]}"; do
  s="${SRCS[$i]}"; o="${OBJS[$i]}"
  base="$(basename "$s")"
  case "$NEEDS_TAG" in *" $base "*) flagsvar=CXXFLAGS ;; *) flagsvar=CXXFLAGS_BASE ;; esac
  flags="${!flagsvar}"
  cmd="[$CXX_BANNER] $CXX $flags $DEPFLAGS -c $s -o $o"
  if why="$(need_rebuild "$o" "$cmd")"; then
    TODO_SRC+=("$s"); TODO_OBJ+=("$o"); TODO_CMD+=("$cmd"); TODO_WHY+=("$why"); TODO_KIND+=(cxx); TODO_FLAGSVAR+=("$flagsvar")
  else
    SKIPPED+=("$(basename "$s")")
  fi
done
for i in "${!EXTRA_C_SRCS[@]}"; do
  s_c="${EXTRA_C_SRCS[$i]}"; flagsvar_c="${EXTRA_C_FLAGSVARS[$i]}"; flags_c="${!flagsvar_c}"
  o_c="$OUT/obj-$(basename "${s_c%.*}").o"
  cmd_c="[$CXX_BANNER] $CC_C $flags_c $DEPFLAGS -c $s_c -o $o_c"
  if why="$(need_rebuild "$o_c" "$cmd_c")"; then
    TODO_SRC+=("$s_c"); TODO_OBJ+=("$o_c"); TODO_CMD+=("$cmd_c"); TODO_WHY+=("$why"); TODO_KIND+=(cc); TODO_FLAGSVAR+=("$flagsvar_c")
  else
    SKIPPED+=("$(basename "$s_c")")
  fi
done
# build_id.cpp: its own flags variable, recomputed above from the
# git commit — see the comment at CXXFLAGS_BUILDID's definition. Its command
# fingerprint changing on every new commit is the mechanism, not a bug: it is
# meant to always look "stale" to need_rebuild() when HEAD moved.
o_bid="$OUT/obj-$(basename "${BUILDID_SRC%.*}").o"
cmd_bid="[$CXX_BANNER] $CXX $CXXFLAGS_BUILDID $DEPFLAGS -c $BUILDID_SRC -o $o_bid"
if why="$(need_rebuild "$o_bid" "$cmd_bid")"; then
  TODO_SRC+=("$BUILDID_SRC"); TODO_OBJ+=("$o_bid"); TODO_CMD+=("$cmd_bid"); TODO_WHY+=("$why"); TODO_KIND+=(cxx); TODO_FLAGSVAR+=("CXXFLAGS_BUILDID")
else
  SKIPPED+=("$(basename "$BUILDID_SRC")")
fi

if [ "${#SKIPPED[@]}" -gt 0 ]; then
  echo "  A JOUR, non recompile (${#SKIPPED[@]}) : ${SKIPPED[*]}"
fi

# Anti-"lying diagnostic" warning: rt_boot.cpp bakes __DATE__/__TIME__ into
# crash.log's first line. Under incremental builds, if it is NOT recompiled,
# that stamp stays the one from its last compile — a log pulled from the
# console would then carry a timestamp earlier than the VPK just produced.
# This is reported, with the actual baked-in time, so log/VPK pairing stays
# possible.
case " ${SKIPPED[*]} " in
  *" rt_boot.cpp "*)
    echo "  NOTE: rt_boot.cpp non recompile — le tampon __DATE__/__TIME__ de crash.log"
    echo "        reste celui du $(date -r "$OUT/obj-rt_boot.o" '+%Y-%m-%d %H:%M:%S'), pas celui de ce build."
    echo "        D2VPK_REBUILD=1 (ou --rebuild) pour le rafraichir."
    ;;
esac

# ---------------------------------------------------------------------------
# Parallel compilation. Each unit writes its return code to a .rc file and
# its output to a .log file: after `wait`, everything is read back. A failed
# unit leaves NEITHER an object NOR a fingerprint, so the next build retries
# it (no false "up to date" after a failure).
# ---------------------------------------------------------------------------
STATE="$OUT/.fastbuild"
rm -rf "$STATE"; mkdir -p "$STATE"

# eval is NOT used: unquoted word-splitting of "$CXX $CXXFLAGS -c ..." lets
# -DD2VPK_TAG="SANSBLIT" reach the compiler WITH its quotes intact (the define
# evaluates to the C string "SANSBLIT"). An eval would eat the quotes and
# turn the define into a bare identifier: a different binary, or a compile
# error.
compile_one() { # $1=index
  local i="$1" s="${TODO_SRC[$i]}" o="${TODO_OBJ[$i]}" k="${TODO_KIND[$i]}"
  local base; base="$(basename "$o" .o)"
  local rc=0
  local flagsvar="${TODO_FLAGSVAR[$i]}" flags; flags="${!flagsvar}"
  if [ "$k" = cxx ]; then
    $CXX $flags $DEPFLAGS -c "$s" -o "$o" > "$STATE/$base.log" 2>&1 || rc=$?
  else
    $CC_C $flags $DEPFLAGS -c "$s" -o "$o" > "$STATE/$base.log" 2>&1 || rc=$?
  fi
  if [ "$rc" = 0 ]; then
    printf '%s' "${TODO_CMD[$i]}" > "${o%.o}.cmd"
    echo 0 > "$STATE/$base.rc"
  else
    # A failure leaves neither object nor fingerprint: the next build retries it.
    rm -f "$o" "${o%.o}.cmd" "${o%.o}.d"
    echo "$rc" > "$STATE/$base.rc"
  fi
}

if [ "${#TODO_OBJ[@]}" -eq 0 ]; then
  echo "  rien a recompiler"
else
  for i in "${!TODO_OBJ[@]}"; do
    echo "  CXX $(basename "${TODO_SRC[$i]}")  [${TODO_WHY[$i]}]"
  done
  running=0
  for i in "${!TODO_OBJ[@]}"; do
    while [ "$running" -ge "$JOBS" ]; do wait -n || true; running=$((running-1)); done
    compile_one "$i" &
    running=$((running+1))
  done
  wait
  # Summary: any unit with no .rc, or a nonzero .rc, is a failure.
  fails=0
  for i in "${!TODO_OBJ[@]}"; do
    base="$(basename "${TODO_OBJ[$i]}" .o)"
    rc="$(cat "$STATE/$base.rc" 2>/dev/null || echo 127)"
    if [ "$rc" != "0" ]; then
      fails=$((fails+1))
      echo "---- ECHEC: ${TODO_SRC[$i]} (rc=$rc) ----" >&2
      cat "$STATE/$base.log" >&2 2>/dev/null || true
    else
      # any warnings: shown anyway
      [ -s "$STATE/$base.log" ] && { echo "---- $(basename "${TODO_SRC[$i]}") ----"; cat "$STATE/$base.log"; } || true
    fi
  done
  if [ "$fails" -gt 0 ]; then
    echo "FATAL: $fails unite(s) de compilation en echec — build interrompu" >&2
    exit 1
  fi
fi
COMPILED="${#TODO_OBJ[@]}"

# ---------------------------------------------------------------------------
# Tail: link + nm guard + velf + fself + sfo + vpk.
# Replayed whenever an object moved, a product is missing, or the .elf is
# older than an object / the dynarec archive / this script.
# ---------------------------------------------------------------------------
ELF="$OUT/d2vita.elf"
tail_needed=0
if [ "$COMPILED" -gt 0 ]; then tail_needed=1; fi
for f in "$ELF" "$OUT/d2vita.velf" "$OUT/eboot.bin" "$OUT/param.sfo" "$OUT/$VPKOUT" "$OUT/nm.txt"; do
  [ -f "$f" ] || tail_needed=1
done
if [ "$tail_needed" = "0" ]; then
  for f in "${OBJS[@]}" "$DYNLIB" "$0"; do
    [ "$f" -nt "$ELF" ] && tail_needed=1 || true
  done
  for f in "$OUT/d2vita.velf" "$OUT/eboot.bin" "$OUT/$VPKOUT"; do
    [ "$ELF" -nt "$f" ] && tail_needed=1 || true
  done
fi

if [ "$tail_needed" = "0" ]; then
  echo "== $OUT/$VPKOUT deja a jour (lien + vpk sautes) =="
  exit 0
fi

echo "== linking eboot =="
#   -u pthread_cancel: emutls.o (libgcc) references pthread only WEAKLY via
#   pthread_cancel's address; without forcing that archive member in,
#   __gthread_active_p folds to constant 0 and emutls becomes a single shared
#   buffer (thread_local goes inert).
#   -u pthread_once: the complementary trap — forcing pthread_cancel makes
#   __gthread_active_p TRUE, so __cxa_guard_acquire (a function-local static)
#   takes the multi-threaded path and locks a mutex initialized by
#   pthread_once... which stays weak and unextracted: the linker NOPs the bl
#   to an undefined weak symbol, the init never runs, and
#   pthread_mutex_lock(NULL) kills the boot BEFORE the memblock. qemu doesn't
#   show this (glibc links pthread strongly). The nm guard below locks down
#   the entire family.
$CXX $CXXFLAGS -Wl,-q "${OBJS[@]}" "$DYNLIB" \
  -lSceDisplay_stub -lSceCtrl_stub -lSceTouch_stub -lSceSysmem_stub -lSceLibKernel_stub \
  -lSceIofilemgr_stub -lSceProcessmgr_stub -lSceKernelThreadMgr_stub -lSceRtc_stub \
  -lSceAppUtil_stub -lSceSysmodule_stub -lScePower_stub -lSceAudio_stub \
  -lSceNet_stub -lSceNetCtl_stub \
  -lSceGxm_stub $SHACC_LD \
  -lSceCommonDialog_stub \
  -ltaihen_stub_weak \
  -Wl,-u,pthread_cancel -Wl,-u,pthread_once $WRAP_LD -lpthread -lm -lz \
  -o "$OUT/d2vita.elf"

# Anti-family guard: a bl to an undefined WEAK pthread symbol is silently
# NOPed by the linker (see -u pthread_once above). Any new gthread reference
# from libstdc++/libgcc must be extracted, not NOPed. nm runs ALONE (not
# piped into an if condition: under set -e, a failing nm there would pass
# silently and the guard would validate anything); its failure stops the
# build, then grep inspects the file.
arm-vita-eabi-nm "$OUT/d2vita.elf" > "$OUT/nm.txt"
if grep -E "^ +w +(pthread_|sem_|sched_yield)" "$OUT/nm.txt" ; then
  echo "FATAL: reference pthread faible non resolue (bl NOPe par le linker) — ajouter -Wl,-u,<symbole>"
  exit 1
fi

# Guard: "the engine does not know its consumers' names".
#
# Same family as the guard above, same failure mode: an unresolved WEAK link
# is NULL, with no error or warning. If engine code targets symbols with a
# consumer-specific prefix (here `d2vita_`), any port whose symbols don't
# carry that prefix silently inherits a MUTE log and UNPINNED threads, while
# still compiling and linking clean. "It compiles" proves nothing here: the
# only proof is in the symbol table, which is why this check lives in the
# build, not in a document.
#
# NEGATIVE leg: no more weak references to a port-specific prefix.
if grep -E "^ +w +(d2vita_|d2rt_[a-z]*_c$)" "$OUT/nm.txt" ; then
  echo "FATAL: reference FAIBLE vers un symbole a prefixe de portage — le moteur"
  echo "       ne doit plus emprunter les symboles de son consommateur (cf. winx86"
  echo "       docs/migration.md, piege 9)."
  exit 1
fi
# POSITIVE leg, essential: silence proves nothing. The negative leg above
# would also pass if the engine's log symbol had vanished from the ELF entirely.
if ! grep -qE "^[0-9a-f]+ T wx86_vita_progress_c$" "$OUT/nm.txt" ; then
  echo "FATAL: wx86_vita_progress_c absent de l'ELF — le journal du moteur n'est"
  echo "       pas lie ; toute ligne de progression serait perdue en silence."
  exit 1
fi

vita-elf-create "$OUT/d2vita.elf" "$OUT/d2vita.velf" >/dev/null
# UNSAFE self (no -s): the runtime needs extended privileges on real hardware
# (sceKernelAllocMemBlockForVM + OpenVMDomain + extended memory). Requires
# "Enable unsafe homebrew" in the HENkaku settings. A -s (safe) self boots on
# Vita3K but can black-screen on a real Vita at the VM allocation.
vita-make-fself "$OUT/d2vita.velf" "$OUT/eboot.bin" >/dev/null

# sfo + vpk
# ATTRIBUTE2=12 requests the extended-memory app mode (+109 MiB) on real
# hardware — the 272 MiB arena + 56 MiB newlib heap need ~332 MiB user RAM.
# Vita3K ignores it; real-Vita behaviour to be confirmed on first hw boot.
vita-mksfoex -s TITLE_ID="$TITLE" -d ATTRIBUTE2=12 "$APPNAME" "$OUT/param.sfo" >/dev/null
# The two .gxp shaders travel INSIDE the VPK (app0:shaders/). psp2cgc isn't
# free software and isn't in the VitaSDK: they're built once, by the
# console's own compiler (libshacccg.suprx) under Vita3K with the
# D2VPK_SHACC=1 flavor, and read back as-is afterward. The shipped VPK
# therefore has NO dependency on SceShaccCg — which matters, because the
# same eboot also runs the GDI path.
# d2_ring_f_flat: the MEASUREMENT variant (D2_GXMPROBE=3, no dependent
# texture read or discard). Also travels in the VPK: without it, the knob
# can't measure anything on a console where libshacccg.suprx isn't installed.
# d2_ring_f_pal (D2_GXMPAL=1, hardware palette) and d2_ring_f_clear
# (D2_GXMCLEAR=plat): two GPU-cost variants kept for the same reason. Like
# the flat probe, they travel in the VPK: on a console without
# libshacccg.suprx, their absence would leave the knobs mute — the backend
# DOES say so and falls back to normal rendering, but nothing would have been
# measured. The ck/palck/half/fixed .gxp variants were dropped along with
# their knobs (regressed, no measurable gain, or broke rendering).
SHADER_ARGS=()
for sh in d2_ring_v d2_ring_f d2_ring_f_flat d2_ring_f_pal d2_ring_f_clear; do
  [ -f "$ROOT/shaders/$sh.gxp" ] && SHADER_ARGS+=(-a "$ROOT/shaders/$sh.gxp=shaders/$sh.gxp")
done
# glide3x.dll: the port's OWN Glide renderer DLL (build-glide/, x86 MinGW). D2
# is launched with -3dfx (d2_boot_config.cpp), so it LoadLibrary's this DLL and
# calls it ~970x/frame. It is NOT a Diablo II file and is absent from a player's
# install, so it MUST travel inside the VPK (app0:glide3x.dll) -- otherwise a
# fresh install halts at frame 0 with "Unsupported graphics mode" (the renderer
# fails to load). Required, not optional: fail the build rather than ship a VPK
# that cannot render. kernel32_modules.cpp loads it from app0: when the game dir
# has none.
GLIDE_DLL="$ROOT/build-glide/glide3x.dll"
[ -f "$GLIDE_DLL" ] || { echo "FATAL: $GLIDE_DLL introuvable — glide3x.dll doit etre embarque dans le VPK, sinon le jeu halte a l'image 0 (mode graphique non supporte)" >&2; exit 1; }
GLIDE_ARGS=(-a "$GLIDE_DLL=glide3x.dll")
# LiveArea (icon, background, splash image). These files live in sce_sys/.
# ⚠️ LiveArea files are NOT tracked by the dependency cache: changing only an
# image will skip the tail and the VPK will keep the old one. Force it with
# D2VPK_REBUILD=1, or repackage by hand with vita-pack-vpk.
LIVEAREA_ARGS=()
for a in "sce_sys/icon0.png" "sce_sys/livearea/contents/bg.png" \
         "sce_sys/livearea/contents/startup.png" "sce_sys/livearea/contents/template.xml"; do
  [ -f "$ROOT/$a" ] && LIVEAREA_ARGS+=(-a "$ROOT/$a=$a")
done
vita-pack-vpk -s "$OUT/param.sfo" -b "$OUT/eboot.bin" "${SHADER_ARGS[@]}" "${GLIDE_ARGS[@]}" "${LIVEAREA_ARGS[@]}" "$OUT/$VPKOUT" >/dev/null
echo "== built $OUT/$VPKOUT =="

# ---------------------------------------------------------------------------
# Crash-report symbol archive + build registration (spec §4.10).
# Never fails the build: a maintainer building offline, or before running
# `tools/crash/crash.py keygen` + admin.env for the first time, still gets a
# VPK — just without the admin tool being able to map this build_id back to
# symbols yet.
# ---------------------------------------------------------------------------
SYMROOT="${D2VCRASH_SYMBOLS_DIR:-$HOME/d2vita-symbols}/$BUILD_ID"
if mkdir -p "$SYMROOT" 2>/dev/null && cp "$OUT/d2vita.elf" "$OUT/nm.txt" "$SYMROOT/" 2>/dev/null; then
  echo "== symboles archives -> $SYMROOT =="
else
  echo "== ATTENTION: echec de l'archivage des symboles dans $SYMROOT (autopsy futur sans symboles pour ce build) =="
fi
if [ -f "$HOME/.config/d2vita-crash/admin.env" ]; then
  if python3 "$ROOT/tools/crash/crash.py" builds register "$BUILD_ID" --version "$VERSION" --channel "$CHANNEL"; then
    :
  else
    echo "== ATTENTION: 'crash builds register' a echoue (hors ligne ? admin.env incomplet ?) — la console enverra quand meme, l'API refusera ce build_id jusqu'a l'enregistrement =="
  fi
else
  echo "== 'crash builds register' saute : ~/.config/d2vita-crash/admin.env absent (voir tools/crash/README.md) =="
fi
