// src/runtime/cdkeys_file.h — CD keys from ux0:data/d2vita/keys.txt
// (see cdkeys_file.cpp and docs-site/en-ligne.md). Without keys.txt: nothing is installed.
#pragma once
namespace d2rt { struct Cpu; class Bridge; }
void cdkeys_hooks_install(d2rt::Cpu* cpu, d2rt::Bridge& br);
