#!/usr/bin/env bash
# tools/hudfill_arm_check.sh — porte qemu du comblage du bandeau (D2_HUDFILL).
#
# Le bandeau n'existe QUE sur le chemin Glide et QUE une fois la bascule
# 960x544 faite (native_hooks_resolution.cpp), donc ni `-w` ni le menu ne
# prouvent quoi que ce soit : ce banc entre en partie avec `-3dfx` et l'anneau,
# puis lit la ligne `hudfill:` que D2_HUDFILL=2 publie a la premiere detection.
#
# PASS = la vignette f4 est reconnue a la geometrie ATTENDUE (960/2+149, 544-64,
# 128x64 — quad complete en puissances de deux) et les deux trous annonces sont
# ceux mesures sur console (165..245 et 715..795). Sous qemu la soumission
# GPU est un puits : ce banc prouve le CROCHET et les NOMBRES, jamais l'image
# — celle-ci demande la console.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4200}"
LOG="${LOG:-/tmp/hudfill_arm.log}"
REFS="${REFS:-/tmp/refs_hudfill_$USER}"
W="${W:-/tmp/hudfill_save}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (tools/build_oracle_arm.sh)"; exit 1; }
[ -f "$ROOT/build-glide/glide3x.dll" ] || bash "$ROOT/tools/build_glide_ring.sh" >/dev/null \
  || { echo "FAIL: DLL anneau"; exit 1; }
mkdir -p "$REFS"; for f in "$DIR"/*; do ln -sf "$f" "$REFS/"; done
rm -f "$REFS/glide3x.dll"; cp "$ROOT/build-glide/glide3x.dll" "$REFS/glide3x.dll"
rm -rf "$W"; mkdir -p "$W/Save"; cp "$ROOT/tools/registry_console.txt" "$W/registry.txt"

# Meme script d'entree en partie que banc_replay60.sh : menu -> barbare ->
# nom VITA -> Entree -> Entree, puis trois deplacements pour rester en jeu.
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
S="$S,3600:move:500:300,3650:ldown:500:300,3660:lup:500:300"
S="$S,4000:move:150:250,4050:ldown:150:250,4060:lup:150:250"

echo "== qemu-arm, Glide, entree en partie — MAXFRAMES=$MAXFRAMES -> $LOG =="
T0=$(date +%s)
GAMEEXE=1 D2ARGS="game.exe -3dfx" WLOG=1 MAXSW=4000000 MAXFRAMES="$MAXFRAMES" \
  D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
  D2_REALCLOCK=1 D2_ROOMGUARD=1 D2_GLIDERING=1 D2_GLIDEGXM=1 \
  D2_HUDFILL="${D2_HUDFILL:-2}" \
  D2WRITE="$W" D2SCRIPT="$S" \
  timeout "${TMO:-3000}" qemu-arm -B 0x10000 "$BIN" "$REFS" > "$LOG" 2>&1
echo "  rc=$? $(( $(date +%s) - T0 ))s"

echo "== verdict =="
fail=0
grep -m1 "res: 960x544 applique" "$LOG" || { echo "FAIL: la bascule 960x544 n'a pas eu lieu"; fail=1; }
L=$(grep -m1 "^hudfill:" "$LOG" || true)
if [ -z "$L" ]; then echo "FAIL: f4 jamais reconnue — aucun trou comble"; fail=1
else
  echo "$L"
  echo "$L" | grep -q "f4 vue en (629.0,480.0) 128x64" \
    || { echo "FAIL: geometrie inattendue (attendu 629,480 128x64)"; fail=1; }
  echo "$L" | grep -q "trous 165..245 et 715..795" \
    || { echo "FAIL: trous inattendus (attendu 165..245 et 715..795)"; fail=1; }
fi
grep -q "CLEAN EXIT" "$W/crash.log" 2>/dev/null || { echo "FAIL: pas de CLEAN EXIT"; fail=1; }
[ $fail -eq 0 ] && echo "PASS" || echo "ECHEC"
exit $fail
