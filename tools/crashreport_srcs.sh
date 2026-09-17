# tools/crashreport_srcs.sh — what the crash reporter is made of, shared by
# the host tests (tools/tests/run_crashreport_tests.sh) and by the Vita
# build (VPK). Sourced, never executed; expects $ROOT to be set.
#
# Build these units and nothing else:
#   CRASHREPORT_SRCS      C++17, platform independent (no VitaSDK header)
#   CRASHREPORT_C_SRCS    C99. The Monocypher unit is src/crashreport/
#                         cr_monocypher.c, NOT third_party/monocypher/
#                         monocypher.c: it sets BLAKE2_NO_UNROLLING itself,
#                         which the 150 KiB .text budget of spec section 4.9
#                         depends on, and building both would define the same
#                         symbols twice.
#   CRASHREPORT_HOST_SRCS the POSIX IoApi and NetApi, for the PC tests only.
#                         The Vita build takes cr_io_vita.cpp and
#                         cr_net_vita.cpp in their place.
#   CRASHREPORT_VITA_SRCS C++17, VitaSDK-only (sceIo/sceNet/sceCtrl/
#                         sceMsgDialog/sceKernel calls): the boot
#                         integration itself — replaces
#                         CRASHREPORT_HOST_SRCS in a VPK build, never built
#                         alongside it. src/crashreport/build_id.cpp is
#                         deliberately NOT in this array: its two string
#                         constants come from -D defines the build script
#                         recomputes on every invocation (see
#                         tools/build_rt_boot_vpk.sh), so it needs its own
#                         compile step with its own command-line fingerprint,
#                         the same way explode.c gets one for being C++ vs. C.
#   CRASHREPORT_INCLUDES  the include flags every unit needs.
#
# zlib is needed too (a psp2dmp is gzip): the Vita build already links it.
# run_crashreport_tests.sh checks that CRASHREPORT_SRCS + CRASHREPORT_HOST_SRCS
# together cover every .cpp of src/crashreport/ EXCEPT the ones listed in
# CRASHREPORT_VITA_SRCS and build_id.cpp (VitaSDK-only: they cannot build for
# the host by construction) — so a new host-testable unit cannot be left out
# of the eboot, and a new Vita-only unit cannot be left out of this list.

CRASHREPORT_SRCS=(
  "$ROOT/src/crashreport/cr_addr.cpp"
  "$ROOT/src/crashreport/cr_api.cpp"
  "$ROOT/src/crashreport/cr_claim.cpp"
  "$ROOT/src/crashreport/cr_crashtxt_parse.cpp"
  "$ROOT/src/crashreport/cr_evidence.cpp"
  "$ROOT/src/crashreport/cr_http.cpp"
  "$ROOT/src/crashreport/cr_json.cpp"
  "$ROOT/src/crashreport/cr_outbox.cpp"
  "$ROOT/src/crashreport/cr_progress_parse.cpp"
  "$ROOT/src/crashreport/cr_psp2dmp.cpp"
  "$ROOT/src/crashreport/cr_redact.cpp"
  "$ROOT/src/crashreport/cr_seal.cpp"
  "$ROOT/src/crashreport/cr_types.cpp"
  "$ROOT/src/crashreport/cr_upload.cpp"
  "$ROOT/src/crashreport/cr_verify.cpp"
)

CRASHREPORT_C_SRCS=(
  "$ROOT/src/crashreport/cr_monocypher.c"
  "$ROOT/third_party/monocypher/monocypher-ed25519.c"
)

CRASHREPORT_HOST_SRCS=(
  "$ROOT/src/crashreport/cr_io_posix.cpp"
  "$ROOT/src/crashreport/cr_net_posix.cpp"
)

CRASHREPORT_VITA_SRCS=(
  "$ROOT/src/crashreport/cr_io_vita.cpp"
  "$ROOT/src/crashreport/cr_net_vita.cpp"
  "$ROOT/src/crashreport/cr_consent_vita.cpp"
  "$ROOT/src/crashreport/cr_boot.cpp"
)

CRASHREPORT_INCLUDES=(-I"$ROOT/src" -I"$ROOT/third_party/monocypher")
