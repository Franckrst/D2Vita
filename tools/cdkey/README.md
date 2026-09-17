# Fichiers de clés CD de Diablo II 1.14d — référence et oracle

Aucune vraie clé ici : tous les tests utilisent des chaînes **factices**, jamais
rendues valides (ce dossier ne sait pas fabriquer de clé qui passe une validation).

- `cdkey_ref.py` — encodeur/décodeur de référence (`python3 cdkey_ref.py` = auto-test).
- `harness.c` — oracle Wine : charge `~/d2-vita-refs/1.14d/Game.exe` et appelle SES
  routines (0x523060 chiffrement, 0x5232b0 vérification, 0x522bc0 mot de passe).
  `i686-w64-mingw32-gcc -O1 -o harness.exe harness.c`
- `oracle_cmp.py` — compare référence et jeu (`unshare -rn wine`, aucun réseau) :
  16 cas PASS.

Implémentation cible : `src/runtime/cdkeys_file.cpp` (vecteur KAT produit par le
jeu, différentiel C++/Python 3000 cas, 0 écart). Détails : `docs-site/en-ligne.md`.
