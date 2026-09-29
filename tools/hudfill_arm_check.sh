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
#
# PERSP=1 (defaut) ou PERSP=0 : l'option video « Perspective » du jeu, forcee
# via D2_PERSPECTIVE. Elle change la FORME du quad : ON, D2 dessine la texture
# completee (f4 = 128x64 en y=H-64) ; OFF, l'art seul (f4 = 86x55 en y=H-55).
# Les bancs tombent sur ON sans que personne l'ait choisi (voir
# win32_shims_advapi32_d2.cpp) : c'est ainsi que OFF a pu casser le comblage
# sans qu'aucune porte ne le voie. Passer les deux.
#
# MODE640=1 : la meme partie avec l'option video Resolution sur 640x480
# (registre Resolution=0). Le jeu dessine alors en 848x480 et c'est la
# disposition 640 qui est verifiee : pierre capturee dans l'inventaire (I),
# trous du bandeau 640 (165..W/2-155 et W/2+155..W-165) et colonne entre les
# deux panneaux (320..W-320). La pierre est lue dans le cel DC6 du jeu des
# l'entree en partie : le bandeau doit etre comble AVANT la touche I.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-5200}"
LOG="${LOG:-/tmp/hudfill_arm.log}"
REFS="${REFS:-/tmp/refs_hudfill_$USER}"
W="${W:-/tmp/hudfill_save}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (tools/build_oracle_arm.sh)"; exit 1; }
[ -f "$ROOT/build-glide/glide3x.dll" ] || bash "$ROOT/tools/build_glide_ring.sh" >/dev/null \
  || { echo "FAIL: DLL anneau"; exit 1; }
mkdir -p "$REFS"; for f in "$DIR"/*; do ln -sf "$f" "$REFS/"; done
rm -f "$REFS/glide3x.dll"; cp "$ROOT/build-glide/glide3x.dll" "$REFS/glide3x.dll"
rm -rf "$W"; mkdir -p "$W/Save"; cp "${REG:-$ROOT/tools/registry_console.txt}" "$W/registry.txt"   # REG=<fichier> : autre registre
[ "${MODE640:-0}" = 1 ] && echo 'software\blizzard entertainment\diablo ii|resolution|Resolution|4|00000000' >> "$W/registry.txt"

# Meme script d'entree en partie que banc_replay60.sh : menu -> barbare ->
# nom VITA -> Entree -> Entree, puis trois deplacements pour rester en jeu.
S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"
S="$S,3600:move:500:300,3650:ldown:500:300,3660:lup:500:300"
S="$S,4000:move:150:250,4050:ldown:150:250,4060:lup:150:250"
# Puis inventaire (I) et personnage (C) ouverts ensemble : aux bords, la
# colonne 400..W-400 entre les deux panneaux doit etre comblee (gx_colonne_fill).
S="$S,4150:chr:27,4200:key:73,4250:key:67"   # chr:27 : ferme le chat ouvert par les Entree

echo "== qemu-arm, Glide, entree en partie — MAXFRAMES=$MAXFRAMES -> $LOG =="
T0=$(date +%s)
GAMEEXE=1 D2ARGS="game.exe -3dfx" WLOG=1 MAXSW=4000000 MAXFRAMES="$MAXFRAMES" \
  D2LAYOUT=compact D2ARENA=12900000 NATIVECELLLOOP=1 \
  D2_REALCLOCK=1 D2_ROOMGUARD=1 D2_GLIDERING=1 D2_GLIDEGXM=1 \
  D2_HUDFILL="${D2_HUDFILL:-2}" D2_PERSPECTIVE="${PERSP:-1}" \
  D2WRITE="$W" D2SCRIPT="$S" \
  timeout "${TMO:-3000}" qemu-arm -B 0x10000 "$BIN" "$REFS" > "$LOG" 2>&1
echo "  rc=$? $(( $(date +%s) - T0 ))s"

echo "== verdict =="
fail=0
if [ "${MODE640:-0}" = 1 ]; then
  grep -m1 "res: 848x480 applique, mode 0" "$LOG" || { echo "FAIL: la bascule 848x480 (mode 640) n'a pas eu lieu"; fail=1; }
  for pat in "res: pierre de l'inventaire lue dans le cel du jeu" "pierre640: figee dans la cellule [0-9]+ \\(cel DC6 du jeu\\)" "hud640: vignette A en \\(269,4(16|25)\\) 128x(55|64) — trous 165..269 et 579..683 combles" \
             "colonne640: colonne 320..528 x 0..432 comblee"; do
    L=$(grep -m1 -E "$pat" "$LOG" || true)
    if [ -z "$L" ]; then echo "FAIL: attendu « $pat »"; fail=1; else echo "$L"; fi
  done
  # Le bandeau doit etre comble AVANT que l'inventaire soit ouvert (touche I).
  nh=$(grep -n -m1 "^hud640:" "$LOG" | cut -d: -f1); ni=$(grep -n -m1 "inj\] frame 4200: key 73" "$LOG" | cut -d: -f1)
  if [ -n "$nh" ] && [ -n "$ni" ] && [ "$nh" -lt "$ni" ]; then echo "bandeau comble avant l'ouverture de l'inventaire (ligne $nh < $ni)"
  else echo "FAIL: bandeau pas comble avant l'ouverture de l'inventaire (hud640 ligne ${nh:-?}, touche I ligne ${ni:-?})"; fail=1; fi
  grep -q "CLEAN EXIT" "$W/crash.log" 2>/dev/null || { echo "FAIL: pas de CLEAN EXIT"; fail=1; }
  [ $fail -eq 0 ] && echo "PASS" || echo "ECHEC"
  exit $fail
fi
grep -m1 "res: 960x544 applique" "$LOG" || { echo "FAIL: la bascule 960x544 n'a pas eu lieu"; fail=1; }
L=$(grep -m1 "^hudfill:" "$LOG" || true)
if [ -z "$L" ]; then echo "FAIL: f4 jamais reconnue — aucun trou comble"; fail=1
else
  echo "$L"
  if [ "${PERSP:-1}" = 0 ]; then GEO="(629.0,489.0) 86x55"; else GEO="(629.0,480.0) 128x64"; fi
  echo "$L" | grep -qF "f4 vue en $GEO" \
    || { echo "FAIL: geometrie inattendue (attendu $GEO, PERSP=${PERSP:-1})"; fail=1; }
  echo "$L" | grep -q "trous 165..245 et 715..795" \
    || { echo "FAIL: trous inattendus (attendu 165..245 et 715..795)"; fail=1; }
fi
C=$(grep -m1 "colonne: barre" "$LOG" || true)
if [ -z "$C" ]; then echo "FAIL: colonne entre les panneaux jamais comblee"; fail=1
else echo "$C"; echo "$C" | grep -q "colonne 400..560" \
       || { echo "FAIL: colonne inattendue (attendu 400..560)"; fail=1; }; fi
grep -q "CLEAN EXIT" "$W/crash.log" 2>/dev/null || { echo "FAIL: pas de CLEAN EXIT"; fail=1; }
[ $fail -eq 0 ] && echo "PASS" || echo "ECHEC"
exit $fail
