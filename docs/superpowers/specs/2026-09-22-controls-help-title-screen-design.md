# Controls-help overlay on the title screen

Status: approved for planning. Branch: `ui/aide-controles-ecran-titre`.

## Goal

Diablo II's own menus are the genuine, unmodified `Game.exe` UI — no button can be
inserted into them without touching the Blizzard binary, which this project never
does (`CLAUDE.md` — "Run Diablo, don't rewrite Diablo"). Instead, this adds a
Vita-side overlay, in the same family as the existing virtual keyboard
(`third_party/winx86/src/platform/vita_kb.h`) and radial menu
(`src/platform/radial_menu.h`): a small persistent hint on the title screen that
opens a translucent panel listing the current effective controller bindings.

Scope is intentionally narrow: the hint and the panel are available **only on the
literal title screen**, not on character select, options, or in a game. See
"Open risk" below — this scope is a hard requirement, not a default that falls
back to something broader if it turns out to be hard.

## Non-goals

- No change to `Game.exe`, no new Win32 shim.
- No localization: the panel is English-only (matches the rest of the Vita-side
  runtime UI — boot log, HUD, existing overlays).
- No attempt to make the hint icon double as a real touch target for anything
  other than opening the panel.

## 1. Title-screen detection — the blocking risk

There is currently no memory signal in this codebase that identifies "the D2
title screen, specifically" as opposed to any other non-gameplay screen
(character select, options, cinematics). The one live, per-frame, reliable
signal that exists today is the player-object pointer read in
`src/runtime/phase_hooks.cpp:437` (`cpu->read_u32(g_d2base + 0x3a6a70)`,
non-null ⇒ a player object exists ⇒ in a game), documented as trustworthy in
`third_party/winx86/src/glide_ring/replay60.h:37-56`. That signal is *too broad*
for this feature: it only tells us "not in a game," which also covers character
select and the options screens — explicitly out of scope here.

**Before any UI work starts**, implementation opens with a debug spike: compare
memory at candidate offsets across the title screen, character select, and the
options screen (console or qemu-arm, following the empirical method the rest of
this codebase's offsets were found with — see `runtime-debugging` skill) to find
one that is true on the title screen and false everywhere else, including
mid-game.

**If no such field is found**, the feature stops here and is reported back
rather than shipped against a broader or approximated signal. Do not fall back
to the "not in a game" signal, and do not guess an offset without empirical
verification on real state transitions.

## 2. Persistent hint icon

- Visible only when the title-screen signal (from step 1) is true.
- Lives in the title screen's left letterbox band: at this stage D2's own art is
  a fixed 800x600 canvas centered in the 960x544 physical screen
  (`native_hooks_resolution.cpp`, `present_scale.cpp:fit_rect`), giving two
  vertical bands D2 never draws into — left band `x∈[0,117)`, full height. The
  icon (small translucent glyph + short label, e.g. "Controls") sits in that
  band. It never overlaps pixels D2 itself renders, so intercepting a touch
  there can never eat a click meant for a real D2 menu button.
- Drawn the same way as the virtual keyboard: CPU-side into the presentation
  framebuffer, using the existing `rect`/`frame`/`text` primitives style from
  `vita_kb.h`'s `draw_detail`, not a GPU atlas layer (no compositing budget
  concern at this size).

## 3. Opening / closing the panel

- **Open**: a still tap inside the icon's hit-rect (front touch panel — already
  mapped to D2's absolute cursor elsewhere in `vita_present.cpp:d2vita_input_tick`
  via `pad_to_game()`; here the tap is consumed before reaching that path when
  its coordinates fall inside the icon's band-confined rect, and only while the
  title-screen signal is true).
- **Close**: pressing Circle (○) or Start while the panel is open. Both are
  consumed (not forwarded to D2) for the duration the panel is open — safe,
  since the panel only exists in a state where D2 wouldn't otherwise care about
  those keys reaching it (title screen).
- The panel is a full-screen translucent overlay (same drawing style as the
  icon), opaque enough to read text over the title screen art beneath it.

## 4. Panel content — sourced live, not duplicated

The panel must reflect **actual current bindings**, including whatever the
player has overridden in `ux0:data/d2vita/controls.txt`. Research found the
binding logic is split in two:

- `g_btn[]` in `src/platform/vita_present.cpp` (`{bit, base_action,
  layer_action}` per physical button) is populated from `controls.txt` at load
  (`load_controls_txt()`, `vita_present.cpp:1635-1657`) and *is* the dispatch
  table — iterating it for display is accurate by construction, no separate
  copy to keep in sync.
- A handful of bindings are hardcoded outside that table and are **not**
  remappable: L/R click, Select → radial menu, R+Select → Space, R+Triangle →
  virtual keyboard, L+Start → screenshot, L+Up → lag marker (diagnostic build
  only). These six are added to the panel's rendering as a fixed tail list,
  clearly documented in-code as the non-remappable set (mirrors the comment
  already at `vita_present.cpp:1547-1552`).

Rendering combines "iterate `g_btn[]`" + "append the fixed six" into the display
list every time the panel opens — never a static English copy of
`docs-site/controles.md` baked in ahead of time, so there is nothing to drift
out of sync when bindings logic changes later.

## 5. Scrolling

Content can exceed one screen. Both input paths scroll the same list offset:

- D-pad up/down, one row at a time (D-pad is otherwise idle on the title
  screen).
- Touch drag (vertical delta while a touch is held, converted to a row-offset
  delta) — same front touch panel used for opening the icon, but only read for
  drag once the panel is already open, so it doesn't compete with the tap that
  opened it.

## 6. Implementation shape

- New header-only module, `src/platform/controls_help.h` (or under
  `third_party/winx86/src/platform/` next to `vita_kb.h` if that's where the
  build expects overlay headers — confirm during implementation), following
  `vita_kb.h`'s shape: a `State` struct written by the input tick, a `draw()`
  read by the presentation path, no VitaSDK types leaking into the header (same
  "benign race, single writer" model documented for `d2kb`/`radial_menu`).
  Actual file location depends on where the title-screen signal from step 1
  ends up living.
- Wiring: icon/panel state updates in `d2vita_input_tick()`
  (`vita_present.cpp`), drawing call alongside `draw_keyboard(fb)` in the
  presentation path.

## 7. Validation

- qemu-arm boot check: build succeeds, boots to title, no crash with the
  overlay wired in (even before the panel is opened).
- Vita3K screenshot: confirms the icon appears on the title screen and
  disappears once a character-select/options/in-game state is reached, and that
  the panel renders and is legible when opened.
- Console: confirms the icon is title-screen-only (not shown at character
  select, options, or in a game), confirms a custom `controls.txt` remap shows
  up correctly in the panel, confirms touch-drag and D-pad scrolling both move
  the same list, confirms Circle and Start both close the panel without leaking
  an Esc/Shift keystroke to D2.

## Open risk (restated)

Section 1's spike is the load-bearing unknown of this whole feature. Everything
else in this design (icon, panel, content, scrolling) is low-risk reuse of
patterns already proven elsewhere in the port. If the spike fails to find a
clean title-screen-only signal, stop and report back — do not ship against the
broader "not in a game" signal or an unverified guessed offset.
