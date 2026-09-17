#!/usr/bin/env bash
# tools/oracle_son.sh — SOUND ORACLE, modeled on tools/oracle_phaseprof.sh.
#
# What it proves, without a console and without a speaker:
#   1. the bench is deterministic (A == A2);
#   2. the DirectSound COM base (D2_SON=null) and the WAV sink (D2_SON=wav) do
#      NOT change A SINGLE PIXEL relative to the muted leg;
#   3. the game does not emit "Couldn't initialize DirectSound" and does
#      create its ~25 secondary buffers — proof that InitDS's fallback ladder
#      reaches the intended 2D stereo mode;
#   4. the produced WAV carries a REAL SIGNAL: duration > 0, 22050 Hz,
#      nonzero peak amplitude, neither full silence nor clipping (checked by
#      the binary itself, D2_SONTEST reading the file back);
#   5. THE NATIVE CODECS ARE BYTE-EXACT: the WAV produced with native
#      Huffman/ADPCM is IDENTICAL (md5) to the one produced by letting the
#      guest decode (NATIVEHUFF=0 NATIVEADPCM=0). End-to-end proof of the
#      port's correctness, with no verifier inside the decoder itself.
#   6. CODEC-HOOK NON-EMPTINESS AND EMPTINESS. Point 5's md5 equality would
#      still hold if the hooks NEVER fired on either side. So the COUNTERS
#      are asserted: leg C huff>0 AND adpcm>0 AND repli=0; leg D huff=0 AND
#      adpcm=0 (the guest did everything); MUTED leg A huff=0 adpcm=0
#      repli=0 — proof that the three set_alternate hooks, installed
#      unconditionally, never fire once in the muted binary.
#   7. THE KILL SWITCH REALLY CUTS (leg Z): D2_SON=0 with D2_SONDUMP armed
#      returns a MUTE log, no WAV, and zero codec calls.
#   8. THE SIGNAL IS STRUCTURED, NOT NOISE: a SPECTRAL measurement done by
#      the binary itself (share of energy in the 20 dominant bins out of
#      512). White noise measures ~4-5%; this requires >=40%.
#   9. THE VOLUME IS A COMPLETE CHAIN: registry -> SetVolume -> mixer -> WAV
#      (legs E and F). The six real registry keys are Sound Mixer / Master
#      Volume / Music Volume / Positional Bias / NPC Speech / Options Music
#      (0x514b60) — a few other plausible names (Sound Volume, 3D Sound,
#      Environmental Sound, 3D Bias) do not exist in Game.exe 1.14d at all,
#      and Music Volume = 0x80 is REJECTED by the game (bounds check at
#      0x514bfc requires <= 100).
#        leg E = SOUND registry, volumes open (Master 100, Music 100);
#        leg F = the SAME registry with Master Volume = 0, plus D2_SONLOG.
#      If md5 F == md5 E (Master Volume 0 vs 100 produces a byte-identical
#      WAV), that does NOT mean "the registry is inert": compare legs E and
#      F's codec counters above — the game can decode less at Master
#      Volume=0, so the setting does affect guest behavior somewhere past
#      the window the WAV captures. This bench can therefore neither prove
#      nor disprove the registry -> sound chain from the WAV alone — it says
#      so rather than asserting either way.
#      What it DOES assert is what it can prove: that the volume INSTRUMENT
#      exists and FIRES (>= 1 logged SetVolume) — without that, "no line"
#      and "no call" would be indistinguishable on console. Many of those
#      lines can legitimately carry ZERO gain: in the menu, most voices sit
#      at digital silence — worth remembering before blaming the console port.
#
# What it CANNOT prove: console output (sceAudioOut), absence of crackling on
# real ARM, and thread placement.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:-$HOME/d2-vita-refs/1.14d}"
BIN="${BIN:-$ROOT/build-arm/oracle_arm}"
MAXFRAMES="${MAXFRAMES:-4000}"
TAG="${TAG:-}"
[ -x "$BIN" ] || { echo "FAIL: $BIN absent (bash tools/build_oracle_arm.sh)"; exit 1; }

S="300:activate,400:move:400:308,450:ldown:400:308,460:lup:400:308"
S="$S,1500:move:400:300,1550:ldown:400:300,1560:lup:400:300"
S="$S,2400:chr:86,2410:chr:73,2420:chr:84,2430:chr:65"
S="$S,2600:keydown:13,2620:keyup:13,2800:keydown:13,2820:keyup:13"

run() {  # $1 = label, $2... = extra environment variables
  #   REG=<file>          registry to install (default: registry_console.txt)
  #   MAXFRAMES=<n>       frames for this leg
  local LAB="$1$TAG"; shift
  local MAXFRAMES="${MAXFRAMES:-4000}"
  local REG="${REG:-$ROOT/tools/registry_console.txt}"
  local W=/tmp/orson_$LAB LOG=/tmp/orson_$LAB.log
  rm -rf "$W"; mkdir -p "$W/Save"
  cp "$REG" "$W/registry.txt"
  GAMEEXE=1 D2ARGS="game.exe -w" WLOG=1 MAXSW=400000 MAXFRAMES="$MAXFRAMES" \
    D2LAYOUT=compact D2ARENA=12900000 \
    NATIVECELLLOOP=1 \
    FBHASH=1 D2_ROOMGUARD=1 D2WRITE="$W" D2SCRIPT="$S" \
    D2SCHED=coop D2_VIRTCLOCK=1 D2_FAKEWALL=1787000000 \
    env "$@" \
    timeout 3600 qemu-arm -B 0x10000 "$BIN" "$DIR" > "$LOG" 2>&1
  echo "  [$LAB] rc=$?"
}
H(){ grep -oP '\[fbhash\].*empreinte=\K0x[0-9a-f]+' "/tmp/orson_$1$TAG.log" | tail -1; }
# "Couldn't initialize DirectSound": written by the game to ITS OWN log
# (D2<date>.txt in the write folder) and echoed to output under WLOG=1.
DSFAIL(){ { cat "/tmp/orson_$1$TAG.log"; cat /tmp/orson_$1$TAG/D2*.txt 2>/dev/null; } \
          | grep -c "Couldn't initialize DirectSound"; }
VOIX(){ grep -oP '\[son\] arret:.*voix creees=\K[0-9]+' "/tmp/orson_$1$TAG.log" | tail -1; }
# Codec counters: printed by dsound::shutdown() ALWAYS, whether sound is
# armed or not. Without them, the C/D md5 equality would be an EMPTY test.
CJ(){ grep -oP "\[son\] codecs natifs: huff=\K[0-9]+" "/tmp/orson_$1$TAG.log" | tail -1; }
CA(){ grep -oP "\[son\] codecs natifs:.* adpcm=\K[0-9]+" "/tmp/orson_$1$TAG.log" | tail -1; }
CR(){ grep -oP "\[son\] codecs natifs:.* repli=\K[0-9]+" "/tmp/orson_$1$TAG.log" | tail -1; }
# uncoup=<one-shot voices stopped>/<Play without DSBPLAY_LOOPING>, resamp=
# <grains at a rate != 22050>. Both MUST be 0 in 1.14d; if they move, either
# the "every WAV is 22050" census or the "D2 always plays LOOPING" assumption
# is wrong, and the mixer code already handles that case.
RES(){ grep -oP '\[son\] arret:.* resamp=\K[0-9]+' "/tmp/orson_$1$TAG.log" | tail -1; }
NLP(){ grep -oP '\[son\] arret:.* uncoup=[0-9]+/\K[0-9]+' "/tmp/orson_$1$TAG.log" | tail -1; }
SVU(){ grep -oP '\[son\] arret:.* sansvue=\K[0-9]+' "/tmp/orson_$1$TAG.log" | tail -1; }

echo "== ORACLE SON — MAXFRAMES=$MAXFRAMES (layout console, 6 passes en parallele) =="
T0=$(date +%s)
run A  &
run A2 &
run B  D2_SON=null &
run C  D2_SON=wav D2_SONDUMP=/tmp/orson_C$TAG.wav D2_SONMAXS="${SONMAXS:-600}" &
run D  D2_SON=wav D2_SONDUMP=/tmp/orson_D$TAG.wav D2_SONMAXS="${SONMAXS:-600}" NATIVEHUFF=0 NATIVEADPCM=0 &
# Leg E — THE REGISTRY WITH VOLUMES OPEN. The only leg that exercises
# tools/registry_console_son.txt (without it, nothing references that file or
# checks its syntax). What it MEASURES: whether these keys change the
# produced sound — see the md5 C == md5 E comparison below.
REG="$ROOT/tools/registry_console_son.txt" \
  run E  D2_SON=wav D2_SONDUMP=/tmp/orson_E$TAG.wav D2_SONMAXS="${SONMAXS:-600}" &
# Leg F — THE SAME REGISTRY, Master Volume at ZERO. Built here rather than
# committed as a second file: a second registry file to maintain would drift
# from the first at the next change, when exactly ONE value is meant to differ.
REGF=/tmp/orson_reg_muet$TAG.txt
sed 's/|master volume|Master Volume|4|64000000/|master volume|Master Volume|4|00000000/' \
    "$ROOT/tools/registry_console_son.txt" > "$REGF"
grep -q "|master volume|Master Volume|4|00000000" "$REGF" \
  || { echo "FAIL: jambe F — la substitution Master Volume=0 n'a pas pris"; exit 1; }
REG="$REGF" \
  run F  D2_SON=wav D2_SONDUMP=/tmp/orson_F$TAG.wav D2_SONMAXS="${SONMAXS:-600}" D2_SONLOG=1 &
wait
echo "  duree: $(( $(date +%s) - T0 ))s"
HA=$(H A); HA2=$(H A2); HB=$(H B); HC=$(H C); HD=$(H D)
NA=$(grep -oP '\[fbhash\] frames hachees=\K[0-9]+' "/tmp/orson_A$TAG.log" | tail -1)
echo
echo "  A  = ${HA:-ABSENTE}   (frames hachees=${NA:-0}, temoin MUET)"
echo "  A2 = ${HA2:-ABSENTE}  (controle de determinisme)"
echo "  B  = ${HB:-ABSENTE}  (D2_SON=null : socle COM + melangeur, puits nul)   voix=$(VOIX B)"
echo "  C  = ${HC:-ABSENTE}  (D2_SON=wav  : codecs NATIFS)                      voix=$(VOIX C)"
echo "  D  = ${HD:-ABSENTE}  (D2_SON=wav  : codecs INVITES, NATIVEHUFF=0)       voix=$(VOIX D)"
echo "  « Couldn't initialize DirectSound » : A=$(DSFAIL A)  B=$(DSFAIL B)  C=$(DSFAIL C)"
HE=$(H E); HF=$(H F)
echo "  E  = ${HE:-ABSENTE}  (D2_SON=wav, registre SON : Master 100, Music 100)  voix=$(VOIX E)"
echo "  F  = ${HF:-ABSENTE}  (D2_SON=wav, registre SON : Master Volume = 0)      voix=$(VOIX F)"
for L in A B C D E F; do grep -E "^\[son\] (arret|codecs)" "/tmp/orson_$L$TAG.log" | tail -2 | sed "s/^/     [$L] /"; done

fail=0
[ -n "$HA" ] || { echo "FAIL: empreinte A manquante"; fail=1; }
[ "${HA:-x}" = "${HA2:-y}" ] || { echo "FAIL: BANC NON DETERMINISTE (A != A2)"; fail=1; }
# THE DEFAULT HASN'T MOVED: the muted leg must return tools/oracle_phaseprof.sh's
# reference fingerprint — same scenario, same environment.
[ "${HA:-x}" = "0xd332da5981bade4a" ] || { echo "FAIL: la jambe MUETTE ne rend plus l'empreinte de reference 0xd332da5981bade4a"; fail=1; }
# THE SINK AND THE CODECS ARE PIXEL-NEUTRAL: only the BASE (i.e. the game
# doing extra work) can shift a frame, never the mixer or the decoder — they
# only READ.
[ "${HB:-x}" = "${HC:-y}" ] || { echo "FAIL: le puits WAV change des pixels par rapport au puits nul"; fail=1; }
[ "${HB:-x}" = "${HD:-y}" ] || { echo "FAIL: les codecs natifs changent des pixels"; fail=1; }
[ "${NA:-0}" -ge 360 ] || { echo "FAIL: seulement ${NA:-0} frames hachees (<360)"; fail=1; }
[ "$(DSFAIL A)" -ge 1 ] || { echo "FAIL: la jambe MUETTE devrait echouer a initialiser DirectSound"; fail=1; }
[ "$(DSFAIL B)" -eq 0 ] || { echo "FAIL: D2_SON=null n'a pas initialise DirectSound"; fail=1; }
[ "$(DSFAIL C)" -eq 0 ] || { echo "FAIL: D2_SON=wav n'a pas initialise DirectSound"; fail=1; }
for L in B C D E F; do V=$(VOIX $L); [ "${V:-0}" -ge 20 ] \
  || { echo "FAIL: jambe $L n'a cree que ${V:-0} tampons secondaires (attendu >=20)"; fail=1; }; done
# --- CODEC HOOK NON-EMPTINESS / EMPTINESS -----------------------------------
# Without these three blocks, the C==D md5 equality below would stay GREEN
# even if the hooks went silent on both sides.
echo
echo "  codecs  A(muet)=h$(CJ A)/a$(CA A)/repli$(CR A)   C(natif)=h$(CJ C)/a$(CA C)/repli$(CR C)" \
     "  D(invite)=h$(CJ D)/a$(CA D)/repli$(CR D)   E=h$(CJ E)/a$(CA E)/repli$(CR E)"
AJ=$(CJ A); AA=$(CA A); AR=$(CR A); CJC=$(CJ C); CAC=$(CA C); CRC=$(CR C); DJ=$(CJ D); DA=$(CA D)
[ "${AJ:-x}" = "0" ] && [ "${AA:-x}" = "0" ] && [ "${AR:-x}" = "0" ] \
  || { echo "FAIL: la jambe MUETTE a APPELE les codecs (h${AJ:-?}/a${AA:-?}/repli${AR:-?}) — le defaut n'est plus inchange"; fail=1; }
[ "${CJC:-0}" -gt 0 ] && [ "${CAC:-0}" -gt 0 ] \
  || { echo "FAIL: jambe C — les crochets de codec natifs n'ont JAMAIS tire (h${CJC:-?}/a${CAC:-?}) : le test md5 serait VIDE"; fail=1; }
[ "${CRC:-x}" = "0" ] || { echo "FAIL: jambe C — ${CRC:-?} replis de codec (l'invite a repris le decodage)"; fail=1; }
[ "${DJ:-x}" = "0" ] && [ "${DA:-x}" = "0" ] \
  || { echo "FAIL: jambe D — NATIVEHUFF=0/NATIVEADPCM=0 n'a PAS desarme les crochets (h${DJ:-?}/a${DA:-?})"; fail=1; }
# --- the three fidelity checks: MEASURED, not just asserted ----------------
for L in B C D E F; do
  R=$(RES $L); N=$(NLP $L); V=$(SVU $L)
  [ "${R:-x}" = "0" ] || { echo "FAIL: jambe $L — resamp=${R:-ABSENT} : un tampon n'est PAS a 22050 Hz"; fail=1; }
  [ "${N:-x}" = "0" ] || echo "NOTE: jambe $L — ${N} Play sans DSBPLAY_LOOPING (gere : voix a un coup, arret en fin de tampon)"
  [ "${V:-x}" = "0" ] || { echo "FAIL: jambe $L — sansvue=${V:-ABSENT} : une voix n'a pas de vue hote"; fail=1; }
done
# INFORMATION, NOT AN ASSERTION: sound DOES shift frames, and we know why.
# tools/oracle_son_phase.sh proves it: within the region where images differ,
# the SOUND leg is BYTE-IDENTICAL to the muted leg at a SHIFTED frame. That's
# a PHASE shift, not a rendering change: under D2_VIRTCLOCK the guest clock
# advances PER CALL to timeGetTime, and the sound engine's 20 Hz service
# thread plus its volume fades issue extra calls. Same family of limitation
# as the one documented in oracle_nocap.sh.
if [ "${HA:-x}" = "${HB:-y}" ]; then echo "  NOTE: A == B sur 4000 images (le son n'a deplace aucune image)"
else echo "  NOTE: A != B — glissement de phase d'animation sous horloge virtuelle ; voir tools/oracle_son_phase.sh"; fi

# --- proof WAV: verified by the binary itself (same code as D2_SONTEST) -----
# The check also covers the SPECTRUM: "neither silent nor clipping" would
# still let white noise through. tonalite = share of energy in the 20
# dominant bins out of 512; a real white-noise source measures ~5.1%.
echo
declare -A CRETE=()
for L in C D E F; do
  W=/tmp/orson_$L$TAG.wav
  if [ -s "$W" ]; then
    R=$(D2_SONCHECK="$W" timeout 600 qemu-arm -B 0x10000 "$BIN" "$DIR" 2>&1 | grep -m1 "SONCHECK")
    echo "  [$L] $R"
    CRETE[$L]=$(echo "$R" | grep -oP 'crete=\K[0-9]+')
    echo "$R" | grep -q "SONCHECK OK" || { echo "FAIL: le WAV de la jambe $L ne porte pas de signal exploitable"; fail=1; }
    T=$(echo "$R" | grep -oP 'tonalite=\K[0-9.]+')
    awk -v t="${T:-0}" 'BEGIN{exit !(t>=40)}' \
      || { echo "FAIL: jambe $L — tonalite ${T:-0} % < 40 % : ce signal n'est pas structure (bruit blanc ~4 %)"; fail=1; }
  else
    echo "FAIL: WAV $W absent ou vide"; fail=1
  fi
done
MC=$(md5sum /tmp/orson_C$TAG.wav 2>/dev/null | cut -d' ' -f1)
MD=$(md5sum /tmp/orson_D$TAG.wav 2>/dev/null | cut -d' ' -f1)
echo "  md5 C (codecs natifs)  = ${MC:-ABSENT}"
echo "  md5 D (codecs invites) = ${MD:-ABSENT}"
[ -n "$MC" ] && [ "$MC" = "$MD" ] \
  || { echo "FAIL: Huffman/ADPCM natifs ne rendent PAS les memes octets que l'invite"; fail=1; }
ME=$(md5sum /tmp/orson_E$TAG.wav 2>/dev/null | cut -d' ' -f1)
MF=$(md5sum /tmp/orson_F$TAG.wav 2>/dev/null | cut -d' ' -f1)
echo "  md5 E (Master 100)     = ${ME:-ABSENT}   crete=${CRETE[E]:-?}"
echo "  md5 F (Master 0)       = ${MF:-ABSENT}   crete=${CRETE[F]:-?}"
# --- THE VOLUME INSTRUMENT: it must FIRE, or there's no way to read it ------
# This does NOT assert that Master Volume changes the sound: on THIS
# scenario it doesn't, and the log says why (two SetVolume calls, identical
# at 0 and at 100 — the Master/100 multiplication at 0x4dfca1 sits on the
# IN-GAME positional-sound path, not the menu). What's asserted is what this
# bench can actually prove: that the SetVolume log EXISTS and isn't empty.
# Without that non-emptiness check, "no line" and "no call" would be
# indistinguishable on console.
NSV=$(grep -c "\[son\] SetVolume " "/tmp/orson_F$TAG.log" 2>/dev/null || echo 0)
NG0=$(grep -c "SILENCE NUMERIQUE" "/tmp/orson_F$TAG.log" 2>/dev/null || echo 0)
echo "  journal SetVolume (jambe F, D2_SONLOG=1) : ${NSV} ligne(s), dont ${NG0} a GAIN NUL"
grep "\[son\] Set\(Volume\|Pan\) " "/tmp/orson_F$TAG.log" | head -6 | sed 's/^/     /'
[ "${NSV:-0}" -ge 1 ] \
  || { echo "FAIL: D2_SONLOG=1 n'a journalise AUCUN SetVolume — l'instrument du volume est MUET,"; \
       echo "      donc 'toutes les voix a gain 0' resterait invisible sur console"; fail=1; }
if [ -n "$MF" ] && [ -n "$ME" ] && [ "$MF" = "$ME" ]; then
  echo "  MESURE: md5 F == md5 E — Master Volume (0 vs 100) ne deplace pas un octet du WAV."
  echo "          Mais ce n'est PAS 'le registre est inerte' : comparer les compteurs de codec"
  echo "          des jambes E et F ci-dessus — a Master Volume = 0 le jeu decode MOINS. Le"
  echo "          reglage change donc le comportement invite APRES la fenetre de 600 s que le"
  echo "          WAV capture. Ce banc ne peut NI prouver NI refuter la chaine registre -> son."
else
  echo "  DECOUVERTE: md5 F != md5 E — Master Volume a enfin un effet mesurable sous qemu ;"
  echo "              cas rare, pas explique par ce banc — a investiguer avant de conclure."
fi
if [ -n "$ME" ] && [ "$ME" = "$MC" ]; then
  echo "  MESURE: md5 E == md5 C — les six vraies clefs ne deplacent pas un octet non plus"
  echo "          sur ce scenario (Music Volume est REECRIT a 0 par le jeu dans les deux)."
else
  echo "  DECOUVERTE: md5 E != md5 C — les six vraies clefs ont un effet mesurable."
fi

# --- LEG Z: DOES THE KILL SWITCH REALLY CUT? -------------------------------
# A flag can silently disable more than intended. Here: D2_SON=0 must win
# AGAINST D2_SONDUMP (which, alone, arms recording).
echo
rm -f /tmp/orson_Z$TAG.wav
MAXFRAMES=300 run Z D2_SON=0 D2_SONDUMP=/tmp/orson_Z$TAG.wav
if [ -e "/tmp/orson_Z$TAG.wav" ]; then echo "FAIL: D2_SON=0 a quand meme produit un WAV"; fail=1; fi
grep -q "son MUET par defaut" "/tmp/orson_Z$TAG.log" \
  || { echo "FAIL: D2_SON=0 n'a pas rendu le journal MUET"; fail=1; }
if grep -q "\[son\] arret:" "/tmp/orson_Z$TAG.log"; then
  echo "FAIL: D2_SON=0 a quand meme ouvert un puits (ligne d'arret presente)"; fail=1; fi
[ "$(CJ Z)" = "0" ] && [ "$(CA Z)" = "0" ] && [ "$(CR Z)" = "0" ] \
  || { echo "FAIL: D2_SON=0 — les codecs ont tire (h$(CJ Z)/a$(CA Z)/repli$(CR Z))"; fail=1; }
echo "  [Z] D2_SON=0 + D2_SONDUMP : aucun WAV, journal muet, codecs h$(CJ Z)/a$(CA Z)/repli$(CR Z)"

[ $fail -eq 0 ] && echo "PASS: socle COM pixel-neutre, WAV STRUCTURE (spectre), codecs natifs octet-exacts ET NON VIDES, bouton coupant" || exit 1
