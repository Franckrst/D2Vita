#!/usr/bin/env bash
# tools/build_glide_ring.sh — builds our x86 "ring buffer" glide3x.dll.
#
# The DLL is TRANSLATED by the dynarec just like Game.exe: the ~970 Glide
# calls per frame become x86->x86 calls (no host/guest crossing). Only init,
# the per-frame flush, and texture uploads cross.
#
# Output: build-glide/glide3x.dll. It is NOT copied automatically into the
# game folder — that's the bench's job, so that a "GDI path" run stays
# strictly unchanged.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-i686-w64-mingw32-gcc}"
command -v "$CC" >/dev/null || { echo "FAIL: $CC absent"; exit 1; }
OUT="$ROOT/build-glide"; mkdir -p "$OUT"
# The DLL is DELETED before compiling: otherwise a failed build would leave the
# previous DLL in place, the `[ -f ]` check would pass, and the bench would run
# on stale code while reporting a green build. Never test for a product's
# existence without having destroyed it first.
rm -f "$OUT/glide3x.dll"
CCRC=0
"$CC" -shared -O2 -m32 -static-libgcc -ffreestanding -fno-builtin \
  -Wall -Wextra -Wno-unused-parameter \
  -I"$ROOT/src/glide_ring" \
  "$ROOT/src/glide_ring/glide3x_ring.c" \
  "$ROOT/src/glide_ring/glide3x.def" \
  -o "$OUT/glide3x.dll" \
  -Wl,--enable-stdcall-fixup -nostartfiles -Wl,-e,_DllMain@12 > "$OUT/build.log" 2>&1 || CCRC=$?
grep -v '^$' "$OUT/build.log" || true
[ "$CCRC" = 0 ] || { echo "FAIL: compilation (rc=$CCRC)"; exit 1; }
[ -f "$OUT/glide3x.dll" ] || { echo "FAIL: pas de DLL"; exit 1; }
echo "== $OUT/glide3x.dll =="
i686-w64-mingw32-objdump -p "$OUT/glide3x.dll" | sed -n '/Export Address Table/,/^$/p' | head -5
echo "exports: $(i686-w64-mingw32-objdump -p "$OUT/glide3x.dll" | grep -cE '^\s+\[ *[0-9]+\] _gr|^\s+\[ *[0-9]+\] _gu')"
