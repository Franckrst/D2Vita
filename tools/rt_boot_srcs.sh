# tools/rt_boot_srcs.sh — the rt_boot translation units, shared by every build
# script (VPK, qemu-arm oracle, profiling). Sourced, never executed; expects
# $ROOT to be set. Add a new module here once instead of in each script.
#
# Two halves so the VPK build can slot its Vita-only platform files between
# them and keep its historical object order (link order stays stable).
#
# The crash reporter is NOT listed here: its units are in
# tools/crashreport_srcs.sh, because one of them is C and because its
# Monocypher unit must be src/crashreport/cr_monocypher.c and never
# third_party/monocypher/monocypher.c. Track G sources that file and adds
# ${CRASHREPORT_SRCS[@]} and ${CRASHREPORT_C_SRCS[@]} to the VPK build.

RT_BOOT_SRCS_HEAD=(
  "$ROOT/tools/rt_boot.cpp"
  "$ROOT/src/glide_ring/replay60.cpp"
  "$ROOT/src/glide_ring/gx_host.cpp"
  "$ROOT/src/runtime/phase_hooks.cpp"
  "$ROOT/src/runtime/cdkeys_file.cpp"
  "$ROOT/src/runtime/lazy_seek.cpp"
  "$ROOT/src/runtime/io_stat.cpp"
  "$ROOT/src/runtime/scripted_input.cpp"
  "$ROOT/src/runtime/text_focus_probe.cpp"
  "$ROOT/src/runtime/path_cache.cpp"
  "$ROOT/src/runtime/d2ini.cpp"
  "$ROOT/src/runtime/exe_identity.cpp"
  "$ROOT/src/runtime/alloc_site.cpp"
  "$ROOT/src/runtime/pristine_audit.cpp"
  "$ROOT/src/runtime/cell_frame_diag.cpp"
  "$ROOT/src/runtime/jit_profile.cpp"
  "$ROOT/src/runtime/toolhelp.cpp"
  "$ROOT/src/runtime/frame_profile.cpp"
  "$ROOT/src/runtime/tick_intrinsic.cpp"
  "$ROOT/src/runtime/cs_intrinsic.cpp"
  "$ROOT/src/runtime/emutls_probe.cpp"
  "$ROOT/src/runtime/select_observe.cpp"
  "$ROOT/src/runtime/scheduler_backend.cpp"
  "$ROOT/src/runtime/eip_time_profile.cpp"
  "$ROOT/src/runtime/tier1_intrinsics_install.cpp"
  "$ROOT/src/runtime/kernel32_files.cpp"
  "$ROOT/src/runtime/kernel32_modules.cpp"
  "$ROOT/src/runtime/kernel32_time.cpp"
  "$ROOT/src/runtime/kernel32_interlocked.cpp"
  "$ROOT/src/runtime/kernel32_fsinfo.cpp"
  "$ROOT/src/runtime/win32_import_remainder.cpp"
  "$ROOT/src/runtime/checkrevision_crypto.cpp"
  "$ROOT/src/runtime/netguard_lock.cpp"
  "$ROOT/src/runtime/kernel32_w_variants.cpp"
  "$ROOT/src/runtime/kernel32_filemapping.cpp"
  "$ROOT/src/runtime/native_hooks_codec.cpp"
  "$ROOT/src/runtime/native_hooks_cellengine.cpp"
  "$ROOT/src/runtime/win32_shims_shell32_d2.cpp"
  "$ROOT/src/runtime/win32_shims_advapi32_d2.cpp"
  "$ROOT/src/runtime/win32_shims_user32_d2.cpp"
)

RT_BOOT_SRCS_TAIL=(
  # The boot-time "missing files" / "invalid game files" screens. Vita-only
  # drawing behind #ifdef __vita__, no-op stubs elsewhere -- so it belongs in
  # the shared list: rt_boot.cpp calls the version screen from the plain
  # (non-#ifdef) monolith path, and a build that leaves this unit out fails
  # to link (the qemu-arm oracle did, from the moment the guard existed).
  "$ROOT/src/platform/install_screen_vita.cpp"
  "$ROOT/src/platform/vita_net.cpp"
  "$ROOT/src/runtime/scomp_pkware.cpp"
  "$ROOT/src/runtime/dcc_native.cpp"
  "$ROOT/src/runtime/d2_intrin_114.cpp"
  "$ROOT/src/runtime/scomp_audio.cpp"
  "$ROOT/third_party/StormLib/src/huffman/huff.cpp"
  "$ROOT/third_party/StormLib/src/adpcm/adpcm.cpp"
)
