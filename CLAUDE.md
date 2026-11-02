# ygol-access

Screen reader accessibility mod for Yu-Gi-Oh! Legacy of the Duelist: Link Evolution (Steam, 64-bit Windows), speaking through Prism.

Progress and per-item notes: `CHECKLIST.md`. Update it when an item is done.

## Where things are
- Game: `D:\SteamLibrary\steamapps\common\Yu-Gi-Oh! Legacy of the Duelist Link Evolution`. The mod installs `xinput9_1_0.dll` and `prism.dll` there. The mod writes `ygol-access.log` there; read it after each test run.
- `src/dllmain.cpp`: proxy `XInputGetState` (the game's only XInput import) and the startup thread.
- `src/speech.cpp`: `Speak()` and `Log()`. All speech goes through `Speak()`, which serializes access to the Prism backend.
- `src/text.cpp`: hook on the UI text layer's draw (`DFX::TLayerText`). Records each string and its clip-space position; `SpeakScreen()` reads what was drawn in the last 150 ms. Layer offsets are in `CHECKLIST.md` item 3 notes.
- `src/menu.cpp`: menu speech (focus, descriptions, highlights, popups, values, card names, story text). How it works and the offsets it uses: `CHECKLIST.md` item 4 notes.
- `src/duel.cpp`: duel speech (engine events, the duel cursor, the card panel, effect choices) and board review. Offsets and what each animation id means in practice: `CHECKLIST.md` item 5 notes.
- `src/cards.cpp`: card summaries from `CARD_Prop.bin`, read from the game's YGO_2020.dat at runtime. Tested by `tests/cards_test.cpp`.
- `src/rtti.cpp`: `FindVTable(".?AVname@ns@@")` and `ClassName(object)` from MSVC RTTI. Prefer RTTI vtables over byte patterns for virtual functions.
- Hotkeys: low-level keyboard hook in `src/dllmain.cpp`. Keys are consumed so the game doesn't see them, only while the game's main window (not its owned text-entry window) is in front. F1 reads the hotkey list (keep it and `README.html` in step when hotkeys change), F5 reads the screen, R the player's name and DP (in the deck editor: the deck's card counts), H both duelists' LP (`src/text.cpp`), I the focused card's full info (`src/menu.cpp`). In a duel (`src/duel.cpp`): F field, U hand, G graveyard, V banished, E Extra Deck, T turn and phase; Alt with F/U/G/V/E for the opponent. The game's default keyboard layout uses arrows, Space, S, Enter, A, Backspace, Esc, B, X, Y, 1-5, Tab, Shifts, Ctrls, PgUp/PgDn, Home, End, Pause; pick new hotkeys outside that.
- `src/scan.cpp`: `FindInGame(name, pattern)` finds code by byte pattern in YuGiOh.exe's `.text` and returns null unless there is exactly one match.
  Every miss (and every `FindVTable` miss) counts towards `GameMisses()`, which the mod speaks at startup; a lookup that is allowed to fail must not go through these.
- `README.html`: the player guide, shipped in the release zip. Releases: `/release` skill, `scripts/release.ps1`, `.github/workflows/`.
- `third_party/prism/`: prebuilt Prism v0.18.3 (Windows x64, dynamic). API docs: `C:\Users\khais\Documents\GitHub\prism\doc\src\api`.
- MinHook v1.3.4 comes from CMake FetchContent (`build/_deps/minhook-src`).
- `extracted/`: every file from `YGO_2020.dat`, made by `tools/extract.py`. Generated; don't edit by hand.
  - `extracted/bin/CARD_*.bin`: card data. Format is described at the top of `tools/cards.py`.
  - `extracted/strings/*.BND`: UI strings (not parsed yet).
  - `extracted/pdui/*.dfymoo`: UI layouts.
- `cards_E.json`: all cards decoded by `tools/cards.py`.
- `tools/patcheck.py <exe> "<pattern>"`: checks a byte pattern matches exactly once, offline, before you put it in the mod. `tools/dumpbytes.py <exe> 0xADDR [n]` prints the bytes to build one from.
- `tools/gamekeys.ps1 -Keys DOWN,RETURN -Filter "speak|duel say"`: sends keys to a game started with `gametest.ps1 -KeepOpen` and prints the new log lines. `tools/screenshot.ps1 out.png` captures the screen (window captures miss the UI overlay).
- Reaching a duel: from the title, Enter x5 (Campaign, Yu-Gi-Oh!, The Awakening of Evil, INF-N8), about 28 Enters through the story to "Story Deck", Enter, then Enter for rock-paper-scissors. Send the last Enters one call at a time: an extra Enter in the duel can land on your Deck, whose menu opens on Surrender. GX duel 1 (a Fusion Summon tutorial): Enter x2, Down, Enter x2, about 24 Enters through the story; the tutorial locks the cursor to what it asks for, and Enter on a hint shown over the Phase Menu closes the menu (reopen with Backspace). If keys stop reaching the game, `gamekeys.ps1 -Click` clicks the title bar first.
- `tools/gametest.ps1 -Wait 90 -WaitFor KAZUKI -Keys RETURN,DOWN,F5 -Shot out.png`: launches the game, waits for the title screen, clicks its title bar for focus, sends keys, screenshots, closes the game, prints the log. Never send Enter or Left/Right on deck contents or shop items in tests: that edits the user's decks or spends their DP.
- Reference: [Arefu/Yu-Gi-Oh-Ex](https://github.com/Arefu/Yu-Gi-Oh-Ex) `docs/` covers this exe's menu, widget, duel and text systems with addresses. It has no clear license, so read it for facts and don't copy its code.

## Working here
- Build, test and install:
  `cmake -S . -B build -G "Visual Studio 18 2026" -A x64`, then
  `cmake --build build --config Release`, `ctest --test-dir build -C Release`, `cmake --install build --config Release`.
  Close the game before installing; Windows locks the loaded DLL.
- Test in game with `tools/gametest.ps1`, then read `ygol-access.log` (lines are timestamped from mod load). NVDA is usually running on this machine. The user may press keys in the game during a test run, so an unexpected key in the log may be theirs.
- Sending keys from a background process needs a real click on the game window first (`gametest.ps1` does this); `SetForegroundWindow` alone fails.
- The exe has no ASLR and always loads at 0x140000000, so addresses are the same on every run. Still find hook targets with `FindInGame` patterns, so a game update fails loudly instead of hooking the wrong code.
- Never call Prism or block in `DllMain` (loader lock). Do startup work on the thread `DllMain` starts.
- Prism takes UTF-8. The game's text is UTF-16, so convert it before calling `Speak()`.
- Python tools read untrusted game data: run them with `python -I`.

## Scope of changes and tests
Commit tests only where the task asks for them or this repository already keeps tests for this kind of change, sized like the neighboring test files. Pre-existing bugs or unrelated improvements you notice go in the summary as follow-ups, not in the change.

## Confirm before
- Deleting or overwriting files in the game folder other than `xinput9_1_0.dll`, `prism.dll` and `ygol-access.log`.
- Enabling online play while the mod is loaded.

## Domain terms
- **Card index**: position of a card in `CARD_Prop.bin` and `CARD_Indx_*.bin`. Index 0 is an empty placeholder.
- **Card id**: the 14-bit id in the low bits of a card's first Prop word. Not the same as the card index.
