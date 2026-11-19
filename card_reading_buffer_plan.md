# Card Reading Buffer Plan

## Goal

Add a reading buffer for the selected card's info so it can be read one chunk at a time with new hotkeys, instead of always being spoken in full by the screen reader.

## Revision

The hotkeys were changed from Ctrl+Up/Down to **Alt+Up/Down**. Ctrl alone moves the game's cursor to the top of a menu, so using Ctrl as the modifier is unsafe even if only the release is seen by the game. Ctrl handling was removed from the hook.

## Built Implementation

Status: **implemented, built, unit-tested and tested in game.**

- The reading buffer covers the entire card info that `I` reads: name, type/stats summary, text and copy count.
- `BuildCardInfo` puts the name, type/attribute line, Level/Rank/Link + ATK/DEF line, card text and copy count in separate sections. Spell and Trap cards use the same sectioning, so a card like Pot of Greed becomes `Pot of Greed`, `Normal Spell`, its text and copies.
- `I` still reads the whole string. Alt+Up/Down read the previous/next chunk.
- The first Alt+Down reads the first chunk; the first Alt+Up reads the last.
- Later presses wrap in both directions.
- Chunks reset when the card info changes, the focus leaves the card, or `I` is pressed.
- No position prefix is spoken.
- Each chunk interrupts the previous one.
- Plain Up/Down and plain Alt still reach the game.
- Dotted card names such as B.E.S. Tetran and D.D. Assailant stay in one chunk.
- The copy count is always its own final chunk.

Built DLL:

```text
D:\workspace\ygol-access\build\Release\xinput9_1_0.dll
```

`prism.dll` is unchanged from `third_party/prism/dynamic/release/bin/prism.dll`.

## Decisions

1. **Buffer content:** the whole card info that `I` already reads (name, type/stats summary, card text and copy count).
2. **Chunking:** explicit lines first, then sentence fallback for long lines.
3. **Position feedback:** none. Chunks are spoken without "2 of 7" or similar.
4. **Hotkeys:** Alt+Up = previous chunk, Alt+Down = next chunk.
5. **Wrapping:** stepping before the first chunk goes to the last, and stepping past the last chunk goes to the first.

## Behavior Before the Change

- `I` calls `MenuSpeakCardInfo()`, which speaks `g_cardInfo`.
- `g_cardInfo` is set by:
  - `MakeCardFocus()` for menu cards.
  - `MenuSetCardInfo()` from the duel cursor/card panel.
- The info already contains the cleaned card text. `CleanForSpeech()` turns the game's `|` line break into `\n`, so real line boundaries are available.
- Hotkeys come from the low-level keyboard hook in `src/dllmain.cpp`. It already models the key and the Alt modifier. Plain Up/Down still go to the game.
- `g_cardInfo` is protected by `g_mutex` and written on the game thread. The hotkey thread can safely read a snapshot.

## Feasibility

Very feasible. No new game offsets, hooks or reverse engineering are needed. The text is already available, and the hotkey hook already has a place to add the key handling. The work is a small pure-text chunker plus reading-buffer state.

## Implementation

### 1. Add a pure chunking module

New files: `src/reading.h` / `src/reading.cpp`

```cpp
std::vector<std::wstring> SplitForReading(std::wstring_view text, size_t softLimit = 120);
size_t StepReadingChunk(size_t index, size_t count, int direction);
```

Rules:

- `BuildCardInfo` inserts line breaks between the name, type/attribute line, Level/Rank/Link + ATK/DEF line, card text and copy count. It splits the summary before `Level`, `Rank`, `Link`, `Scale` or `ATK`.
- Split on explicit line breaks first.
- Trim chunks and drop empty ones.
- If a line is longer than the 120-character soft limit, split it at sentence endings (`.`, `!`, `?` followed by whitespace).
- Punctuation inside a quoted card name is not a sentence boundary.
- If a sentence is still too long, split at the last space before the limit, unless that space is inside a quoted card name. In that case cut before the opening quote, or extend past the closing quote when there is no earlier space, so the name stays whole. A chunk may exceed the soft limit only to keep a quoted name whole.
- Quotes recognized by the splitter are `"` and the curly double quotes `U+201C`/`U+201D`.
- Preserve punctuation so speech prosody stays correct.
- `StepReadingChunk` wraps: first + previous -> last, last + next -> first.

This keeps normal card-text lines intact while preventing one enormous paragraph from being read as a single chunk.

Add `tests/reading_test.cpp` and a `reading_test` target in `CMakeLists.txt` for:

- newline splitting
- long-line sentence fallback
- empty lines
- wrap in both directions
- quoted card names cut before the opening quote
- quoted names with no earlier space extended past the closing quote
- a full stop inside a quoted name not ending a sentence
- the real Ra sentence with `"The Winged Dragon of Ra - Sphere Mode"` kept in one chunk
- the card-info sections: name, attributes, stats, text and copies
- dotted names such as B.E.S. Tetran and D.D. Assailant kept whole
- the B. Skull Dragon materials line no longer joining the name and stats
- copies as the final chunk

### 2. Add reading-buffer state in `menu.cpp`

Since the buffer uses the entire card info, no separate card-text state or `MenuSetCardInfo()` signature change is needed.

Add to the existing `g_mutex`-protected state:

```cpp
static uint64_t g_cardVersion;

static std::vector<std::wstring> g_readChunks;
static size_t g_readIndex;
static bool g_readStarted;
static uint64_t g_readVersion;
```

Changes:

- Bump `g_cardVersion` whenever `g_cardInfo` is set or cleared:
  - `MakeCardFocus()` / `g_cardInfo = std::move(info)`
  - `MenuSetCardInfo()`
  - the clear in the announcer when focus leaves cards
- Add:

  ```cpp
  void MenuReadCardChunk(int direction);  // -1 = previous, +1 = next
  ```

Behavior:

- Lazily build chunks from `g_cardInfo` the first time Alt+Up/Down is pressed.
- First Alt+Down reads the first chunk; first Alt+Up reads the last chunk.
- Later presses move by one and wrap.
- If the card changed since the chunks were built, rebuild and reset position.
- If no card is selected, say "No card selected".
- If the info produces no chunks, say "No card text" or equivalent fallback.
- Speak each chunk with `SpeakW(chunk, true)` so a new press interrupts the previous chunk.
- Do not prefix chunks with position information.

`MenuSpeakCardInfo()` still reads the full `g_cardInfo` for `I`. It resets the chunk position when pressed, so the next Alt+Down starts from the beginning.

### 3. Hotkeys in `src/dllmain.cpp`

Alt+Up/Down are now mod hotkeys. Implemented in the hook:

- Read Alt from `LLKHF_ALTDOWN` in the keyboard hook.
- Treat Alt+Up/Down as a mod hotkey only while Alt is held and the game has focus.
- Consume those two combinations so the game never sees them.
- Leave plain Up/Down and plain Alt untouched.
- Keep the existing `WM_APP_HOTKEY` payload as a boolean Alt flag. The existing mask key (`0xE8`) after Alt hotkeys also covers Alt+Up/Down, preventing Alt release from opening the window's system menu.
- Reset the `down[]` auto-repeat tracking on every key-up, not only hotkey key-ups. Otherwise releasing Alt before Up would leave the Up key marked as down and the next Alt+Up would not fire.
- Report Alt+Up/Down to `MenuAnyKeyPressed(false)` because the game did not see the key. This matters for the duel cursor's "only speak after a game key" logic.
- Add `MenuReadCardChunk(-1)` / `MenuReadCardChunk(+1)` to the message loop.
- Update the `F1` help text.

### 4. Documentation

- Add Alt+Up/Down to the hotkey table in `README.html`.
- Update the hotkey list in `CLAUDE.md`.
- Add notes to `CHECKLIST.md`.
- Do not edit `CHANGELOG.md`; this repo writes release notes separately.

## Validation

### Automated

Configured and built with Visual Studio Build Tools 2022 using the `Visual Studio 17 2022` CMake generator (x64 Release).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`ctest --test-dir build -C Release` passed all 3 tests:

- `scan_test`
- `cards_test`
- `reading_test`

`reading_test` covers line splitting, long-line sentence fallback, empty lines, wrapping, quoted names, punctuation inside quotes, the Ra/Sphere Mode case, card-info sections, dotted names, the B. Skull Dragon materials case and final copies.

### In game

Tested by the user:

- Alt+Up/Down read chunks correctly.
- Alt alone has no side effect.
- The first Alt+Down reads the first chunk; the first Alt+Up reads the last.
- Wrapping works in both directions.
- A long card (`The Winged Dragon of Ra - Immortal Phoenix`) produced no overly long chunks.
- A retest after the splitter improvement found no issues.
- After the sectioning change, the user tested a large number of cards and confirmed every improvement.
- Spell cards also benefit: `Pot of Greed` was previously one chunk containing its name, `Normal Spell` and text; it now splits into name, type, text and copies.

### Quoted-name verification

The compiled splitter was run against the exact card text logged by the game for these cards:

| Card | Chunks | Result |
|---|---:|---|
| Ancient Gear Box | 7 | Both `"Ancient Gear Box"` occurrences stayed whole; name, attributes and stats are separate |
| Avatar of the Pot | 5 | `"Pot of Greed"` stayed whole; copies last |
| Archfiend Commander | 9 | Name, `Effect monster, DARK Fiend`, and `Level 6, ATK 2500, DEF 1200` are separate; all quoted names whole |
| Arisen Gaia the Fierce Knight | 9 | Both `"Black Luster Soldier"` occurrences and `"Arisen Gaia the Fierce Knight"` stayed whole; copies last |
| Chimera the Flying Mythical Beast | 8 | Every quoted name stayed whole, including the two Fusion materials; copies last |
| The Winged Dragon of Ra - Immortal Phoenix | 12 | Both Ra names stayed whole; the final sentence splits before `"The Winged Dragon of Ra - Sphere Mode"`; copies last |
| B. Skull Dragon | 6 | Name, attributes, stats, materials and the Archfiend note are separate; copies last |
| Battlecruiser Dianthus | 7 | Name, attributes, `Rank 3, ATK 2100, DEF 1800`, materials and effect text are separate; copies last |
| B.E.S. Tetran | 9 | The dotted name stays whole; attributes and stats are separate; copies last |
| D.D. Assailant | 6 | The dotted name stays whole; attributes and stats are separate; copies last |
| Banner of Courage | 4 | Name, `Continuous Spell`, text and `0 copies` are separate |

Every generated chunk had balanced quote characters. Some long sentences still split at unquoted spaces, which is the intended space fallback.

## Risks and Outcomes

- **Alt+Up/Down conflict:** tested in game; no conflict found, and Alt alone has no side effect.
- **Card data without line breaks:** verified with real long card text; sentence and space fallbacks work.
- **Quoted card names:** verified across the cards above; quoted names stay whole in one chunk.
- **Dotted card names:** fixed by keeping the name in its own section, so `B.E.S. Tetran` and `D.D. Assailant` stay whole.
- **Merged name/summary/card text:** fixed by sectioning the card info instead of relying on the first line break in the card text. This also fixes Spell and Trap cards such as Pot of Greed.
- **Copies joined to the last text chunk:** fixed by making the copy count its own final section.
- **Speech interruption:** accepted; each chunk interrupts the previous one, consistent with `I`.
- **Unmatched quotes:** if card data has an unmatched quote, the splitter extends the chunk to the end rather than cutting inside the presumed name. This was not observed.

## Files Changed

- `src/reading.h` (new)
- `src/reading.cpp` (new)
- `src/menu.cpp`
- `src/menu.h`
- `src/duel.cpp`
- `src/dllmain.cpp`
- `CMakeLists.txt`
- `tests/reading_test.cpp` (new)
- `README.html`
- `CLAUDE.md`
- `CHECKLIST.md`
