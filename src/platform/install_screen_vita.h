// src/platform/install_screen_vita.h -- boot-time "missing file(s)" and
// "invalid game files" screens.
//
// Called from tools/rt_boot.cpp's install preflight when Game.exe or a
// required MPQ isn't where it should be. No-op off Vita (nothing to draw
// to) and if `missing` is empty. See src/platform/install_screen_vita.cpp
// for why this needs no sceGxm context.
#pragma once
#include <string>
#include <vector>

void d2vita_show_missing_files_screen(const std::string& dir, const std::vector<std::string>& missing);

// Shown when Game.exe is not the official Diablo II: LoD 1.14d build (see
// src/runtime/exe_identity.h for how that is decided), BEFORE the file is
// loaded: the port patches Game.exe at 1.14d offsets, so any other build --
// a 1.14a/b/c monolith, a 1.13c split-install launcher, a repacked file --
// fails later in some unrelated, unexplained way (D2's own "Unsupported
// graphics mode" dialog at frame 0 on a 1.14b, for one). `found` is the one
// line that names what was actually there (d2exe::screen_line). No-op off
// Vita. Same sceGxm-free framebuffer as above.
void d2vita_show_version_error_screen(const std::string& dir, const std::string& found);

// Shown when a required file IS present and structurally valid (a real MPQ
// header, tables in bounds) but its size is off from the genuine 1.14d
// reference by more than the tolerance in tools/rt_boot.cpp's kSizeHints --
// e.g. a patch_d2.mpq from a different patch level. Unlike the two screens
// above, this is NOT necessarily fatal (the boot can proceed), but it must
// not be a log line nobody reads either: the player who filed the bug report
// this exists for (Discord Repport-001) never looked at boot_progress.txt.
// So it blocks here and requires an explicit Cross press to force the
// startup through -- no auto-continue-after-a-timeout like the screens
// above. No-op off Vita, or if `warnings` is empty. Same sceGxm-free
// framebuffer as above.
void d2vita_show_version_warning_screen(const std::string& dir, const std::vector<std::string>& warnings);
// kubridge (kernel plugin) not loaded: D2Vita still runs, with a 16 MiB JIT
// pool and no game-side crash report. Says so once at boot; Cross continues,
// and so does a 10 s timeout (an optional plugin must not gate the game).
void d2vita_show_kubridge_notice_screen();
