// src/platform/install_screen_vita.h -- boot-time "missing file(s)" screen.
//
// Called from tools/rt_boot.cpp's install preflight when Game.exe or a
// required MPQ isn't where it should be. No-op off Vita (nothing to draw
// to) and if `missing` is empty. See src/platform/install_screen_vita.cpp
// for why this needs no sceGxm context.
#pragma once
#include <string>
#include <vector>

void d2vita_show_missing_files_screen(const std::string& dir, const std::vector<std::string>& missing);

// Shown when Game.exe is a 1.13c / split install (it imports the D2 split DLLs
// Storm/Fog/D2Win/...), which this build does not support -- instead of the
// cryptic "unshimmed Storm.dll ordinal" stop it would otherwise hit a few
// seconds into boot. No-op off Vita. Same sceGxm-free framebuffer as above.
void d2vita_show_version_error_screen(const std::string& dir);
