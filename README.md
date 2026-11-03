# ygol-access

A screen reader accessibility mod for [Yu-Gi-Oh! Legacy of the Duelist: Link Evolution](https://store.steampowered.com/app/1150640/YuGiOh_Legacy_of_the_Duelist_Link_Evolution/) (Steam, 64-bit Windows). It reads the menus, the campaign's story, the deck editor, the shop and duels aloud through [Prism](https://github.com/ethindp/prism), which speaks through NVDA, JAWS and other screen readers, or Windows speech, and adds hotkeys for reviewing the duel board.

**Players:** download the zip from [Releases](https://github.com/Leos-Khai/ygol-access/releases). The player guide, with install steps, every hotkey and what is read, is [`README.html`](README.html), also in the zip. What changed in each version: [`CHANGELOG.md`](CHANGELOG.md).

## How it works

The game loads `xinput9_1_0.dll` from its own folder before the system one, so the mod is a proxy for that DLL. It forwards `XInputGetState`, the only function the game imports from it, and on a startup thread it:

- finds the game's code by byte patterns and MSVC RTTI (never fixed addresses), and hooks it with MinHook: the UI text layer's draw, widget focus, popups, the duel's animation events, the duel cursor and the card panel;
- turns what it sees into speech through Prism;
- installs a low-level keyboard hook for its hotkeys, which the game never sees.

| File | What it does |
| --- | --- |
| `src/dllmain.cpp` | The proxy export, startup, hotkeys, a fix for the game's freeze on quit |
| `src/speech.cpp` | `Speak()` and `Log()` (Prism, and `ygol-access.log` in the game folder) |
| `src/text.cpp` | The text layer hook: every string drawn and where; F5 and H |
| `src/menu.cpp` | Menu speech: focus, descriptions, values, popups, cards, story text |
| `src/duel.cpp` | Duel speech: engine events, the cursor, prompts, materials, board review |
| `src/cards.cpp` | Card summaries from the game's `CARD_Prop.bin` |
| `src/scan.cpp`, `src/rtti.cpp` | Finding code by pattern and classes by RTTI; counts what is not found |
| `CHECKLIST.md` | Progress, plus the addresses, offsets and findings behind each feature |

## Building

Requirements:

- Windows, x64.
- Visual Studio 2022 or 2026 with the "Desktop development with C++" workload. The code is C++20.
- CMake 3.28 or newer.
- Git: CMake fetches MinHook 1.3.4 at configure time. Prism 0.18.3 is prebuilt in `third_party/prism/`.

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The mod is `build/Release/xinput9_1_0.dll`. The runtime library is linked statically, so players need no VC++ redistributable.

To install into the game, close the game first (Windows locks the loaded DLL), then:

```
cmake --install build --config Release
```

This copies `xinput9_1_0.dll` and `prism.dll` to `GAME_DIR`. If your game isn't in `D:\SteamLibrary`, set it once when configuring:

```
cmake -S . -B build -A x64 -DGAME_DIR="C:/Program Files (x86)/Steam/steamapps/common/Yu-Gi-Oh! Legacy of the Duelist Link Evolution"
```

`scripts/package_release.ps1` builds, runs the tests and makes the release zip in `dist/`.

## Testing

- `ctest` runs the unit tests in `tests/`: the pattern scanner and the card summaries.
- In the game: install, play, then read `ygol-access.log` in the game folder. It logs every string drawn, every focus change, the duel events and everything spoken, with timestamps from mod load.
- `tools/gametest.ps1` launches the game, sends keys and prints the log; `tools/gamekeys.ps1` sends keys to a running game. Both use the default Steam path; edit `$game` / `$log` at the top for yours. Be careful what keys you send: Enter or Left/Right on deck contents or shop items edits your decks or spends your DP.

## Research tools

Not needed to build. They need Python 3 (run them with `python -I`, since they read game data).

- `tools/extract.py` unpacks the game's `YGO_2020.dat` into `extracted/`; `tools/cards.py` decodes the card files.
- `tools/patcheck.py` checks that a byte pattern matches YuGiOh.exe exactly once; `tools/dumpbytes.py` prints bytes to build one from.

The game's files and the data extracted from them are kept out of the repository (`.gitignore`): they are Konami's, and each contributor makes their own from their copy of the game.

## Contributing

Bug reports and pull requests are welcome.

**Reporting a problem:** open an [issue](https://github.com/Leos-Khai/ygol-access/issues). Say where you were in the game, what you pressed, what you heard and what you expected, and attach `ygol-access.log` from the game folder (it is overwritten each time the game starts, so copy it right after the problem).

**Pull requests:**

- Find game code with `FindInGame` patterns or `FindVTable`, never fixed addresses, so a game update fails loudly instead of hooking the wrong code. Check new patterns with `tools/patcheck.py`.
- Don't call Prism or block in `DllMain` (loader lock); do startup work on the startup thread.
- New hotkeys must not clash with the game's keyboard layout (arrows, Space, S, Enter, A, Backspace, Esc, B, X, Y, 1-5, Tab, Shift, Ctrl, PgUp/PgDn, Home, End, Pause). Add them to the F1 help in `src/dllmain.cpp` and to `README.html`.
- Test the change in the game and say in the PR what you checked and what you heard. Add a unit test where the code can run outside the game.
- Write down what you learned about the game (addresses, offsets, what an event id means) in the matching section of `CHECKLIST.md`.
- Never commit game files or data extracted from them.
- Don't edit `CHANGELOG.md`; the release notes are written when a version is released.
- The mod hasn't been tested in online duels; if your change touches them, say how you tested it.

Every push and pull request is built and tested by GitHub Actions (`.github/workflows/build.yml`).

## Releases

Maintainers release with `scripts/release.ps1` (or the `/release` Claude Code skill): it turns `CHANGELOG.md`'s Unreleased section into the version's notes, bumps the version in `CMakeLists.txt`, tags and pushes. The tag starts `.github/workflows/release.yml`, which builds the zip and publishes the GitHub release.

## License

[MIT](LICENSE), copyright Khai Shern, Toh.

Prism and the libraries it includes keep their own licenses (MPL-2.0, LGPL-2.1 and others); see `third_party/prism/LICENSES` and `third_party/prism/NOTICE`. MinHook is BSD-2-Clause.

Yu-Gi-Oh! and Legacy of the Duelist are trademarks of their owners. This project is not affiliated with or endorsed by Konami. You need your own copy of the game.
