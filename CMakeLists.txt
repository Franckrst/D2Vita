cmake_minimum_required(VERSION 3.16)

project(d2vita
  VERSION 0.1.0
  DESCRIPTION "Diablo II: LoD on PS Vita via a Box86-derived x86->ARMv7 runtime"
  LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# NOTE: the SHIPPING build does NOT go through CMake.
#
#   third_party/winx86/build.sh  -> the generic engine static lib (ARM / Vita)
#                                    — dynarec + PE32 loader + Bridge/Cpu +
#                                    schedulers (2026-09-10: extracted to the
#                                    standalone winx86 repo, consumed here as
#                                    a submodule; see third_party/winx86/README.md)
#   tools/build_rt_boot_vpk.sh   -> the on-device VPK (rt_boot + D2-specific
#                                    runtime, linked against libwinx86*.a)
#   qemu-arm desktop repro       -> see docs/ (arm-linux-gnueabihf-g++ on
#                                    tools/rt_boot.cpp +
#                                    src/platform/vita_net.cpp + libwinx86.a)
#
# CMake here only builds host-side DEV UTILITIES: the MPQ inspection tools
# (D2-specific, StormLib-based) and the PE inspection tool (generic, comes
# from the winx86 submodule now). The native-reimplementation and AOT-lifting
# attempts were removed (see git history / ADR 004): the runtime approach in
# third_party/winx86/src/runtime + third_party/winx86/src/dynarec86 (generic)
# plus tools/rt_boot.cpp + src/runtime/*.cpp (D2-specific) is the only live
# path.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra)

# -- StormLib (vendored submodule) — MPQ reader for the host-side tools --------
set(BUILD_SHARED_LIBS           OFF CACHE BOOL "" FORCE)
set(STORM_SKIP_INSTALL          ON  CACHE BOOL "" FORCE)
set(STORM_USE_BUNDLED_LIBRARIES ON  CACHE BOOL "" FORCE)
set(WITH_BUNDLED_LIBTOMMATH     ON  CACHE BOOL "" FORCE)
set(WITH_BUNDLED_LIBTOMCRYPT    ON  CACHE BOOL "" FORCE)
set(STORM_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/StormLib EXCLUDE_FROM_ALL)

# -- winx86 (vendored submodule) — generic PE32 loader + import bridge +
#    dynarec + schedulers. Defines winx86_core (portable slice: PE32 loader +
#    Bridge, for desktop checks without a CPU backend) and pe_analyze.
add_subdirectory(third_party/winx86)

# -- Host-side MPQ inspection tools (D2/Blizzard-specific, stay here) --------
foreach(t mpq_grep mpq_probe mpq_hex mpq_extract mpq_put mpq_rm)
  add_executable(${t} tools/${t}.cpp)
  target_link_libraries(${t} StormLib::storm)
endforeach()
