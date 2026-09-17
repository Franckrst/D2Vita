// kernel32_filemapping.h -- real file mapping for the CLASSIC in-Game.exe
// checkrevision (which memory-maps the game executables to hash them; NOT
// the lockdown DLL, which imports none of these): CreateFileMappingA/W,
// MapViewOfFile, UnmapViewOfFile. Kept together: they share the mapping
// table (g_fmaps/FMap) and the VA-arena bookkeeping (g_vaA, g_fmapN/g_fmapB/
// g_unmapN/g_unmapB).
#pragma once
namespace d2rt { class Bridge; }

void kernel32_filemapping_install(d2rt::Bridge& br);
