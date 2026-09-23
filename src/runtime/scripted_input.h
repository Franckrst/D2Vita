// scripted_input.h -- D2SCRIPT scripted input injection (drive the menu
// without a real mouse) + D2CMDFILE runtime command injection. See
// scripted_input.cpp for the full rationale. g_hwnd/g_wndProc/g_msgQ/g_injN
// stay in tools/rt_boot.cpp (win32_shims_user32_d2.cpp also needs them, see
// runtime/rt_host.h).
#pragma once
#include <cstdint>
#include <string>

extern bool g_snap;                     // one-shot frame dump request (D2SCRIPT "snap")
extern uint8_t g_keyState[256];         // VK states driven by injected input

extern "C" { __attribute__((weak)) void d2vita_input_tick(void); }   // Vita physical-input tick (weak)
extern "C" void d2vita_vpad_get(uint32_t* buttons, uint8_t axes[4]);   // virtual pad (padb/pada commands)

void win_activate_once();               // WM_ACTIVATEAPP/ACTIVATE/SETFOCUS, idempotent
void inj_parse(const char* s);          // parses D2SCRIPT into the scheduled event list
unsigned long inj_count();              // number of scheduled D2SCRIPT events (g_inj.size())
void inj_queue(const std::string& act, int a, int b);   // queues/executes one action now
void inj_set_bounds(int w, int h);      // cursor clamp = the GAME size (800x600 until told otherwise)
void inj_tick(int frame);               // fires scheduled D2SCRIPT events due at this frame
void cmd_poll();                        // polls $D2CMDFILE for newly-appended actions
