// src/crashreport/cr_consent.h — the boot-time consent dialog (spec §4.3).
// Implemented by cr_consent_vita.cpp; see that file for why sceMsgDialog is
// not the primary path (a native fallback screen is drawn instead).
#pragma once
#include <cstdint>
#include <string>

namespace d2cr {

enum class ConsentAnswer {
  Later,        // no answer within the time budget: stays pending, ask again next crash
  Send,         // X — send this once
  DontSend,     // O — discard every pending report
  AlwaysSend,   // Triangle — send this, and remember the choice (consent.txt)
};

struct ConsentPrompt {
  bool hang = false;          // true: "seems to be stuck" wording (kind == Hang)
  std::string install_id;     // shown in small print under the dialog
};

// Blocks for at most ~kConsentTimeoutMs waiting for an answer (X / O /
// Triangle), Later on timeout. The caller (cr_boot.cpp) has ALREADY refused
// to call this at all when the ABSOLUTE RULE applies (D2SCRIPT/D2CMDFILE
// non-empty); this function checks the same rule again on its own (belt and
// suspenders — see cr_boot.h::should_suppress_dialog) and returns Later
// immediately rather than trust every future call site to remember.
//
// try_msg_dialog selects whether sceMsgDialog is attempted first
// (D2_CRASHREPORT_MSGDIALOG=1) before falling back to the native vita_kb.h
// screen; see cr_consent_vita.cpp for why this defaults to false.
ConsentAnswer show_consent(const ConsentPrompt& prompt, bool try_msg_dialog);

constexpr uint32_t kConsentTimeoutMs = 60000;

}  // namespace d2cr
