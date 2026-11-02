# ygol-access checklist

Screen reader mod for Yu-Gi-Oh! Legacy of the Duelist: Link Evolution (Steam, 64-bit, custom C++ engine, D3D11, Wwise).

## 1. Loader and speech
- [x] Proxy `xinput9_1_0.dll` that forwards `XInputGetState` (the only XInput import) to the system DLL
- [x] Speech through Prism (best available backend: NVDA, JAWS, then SAPI/OneCore)
- [x] Speak "mod loaded" at startup
- [x] Log file `ygol-access.log` next to the game exe
- [x] Build and install into the game folder with CMake

## 2. Tooling and research
- [x] Unpack `YGO_2020.dat` using `YGO_2020.toc` (`tools/extract.py`, output in `extracted/`)
- [x] Parse the card files: names, text and stats (`tools/cards.py`, export in `cards_E.json`)
- [x] Add MinHook and a byte-pattern scanner (no hardcoded addresses) (`src/scan.cpp`, test in `tests/scan_test.cpp`)

Notes:
- The exe has no ASLR and always loads at 0x140000000, so addresses are the same on every run.
- Renderer setup: 0x1408c62a0. Input polling: 0x1408c4f80. Steam init: 0x1408d6b00.
- XAudio2 is loaded at runtime, not imported. All audio goes through Wwise, whose API is exported by name.
- [Arefu/Yu-Gi-Oh-Ex](https://github.com/Arefu/Yu-Gi-Oh-Ex) `docs/` documents many of this exe's systems with addresses (menus, widgets, duel state, text tables). Read it before items 3-5. The repo has no clear license, so use it as reference only.
- UI strings are probably in `extracted/strings/*.BND` (not parsed yet).
- In-game card lookups will use the game's own loaded card tables or a C++ port of `cards.py` (item 5).

## 3. Text capture
- [x] Find the engine's draw-text function: `DFX::TLayerText` vtable slot 1, 0x14075DE10 (`src/text.cpp`)
- [x] Hook it and log every new or changed string with its position
- [x] Hotkey to read all text currently on screen: F5 (low-level keyboard hook, hidden from the game)

Notes:
- All UI text goes through `DFX::TLayerText` (vtable 0x140A4E798). Its glyph draw 0x140765C00 has no other caller. Card faces render text into textures separately (0x140766540); card data covers that.
- Layer fields: +0xC8 `std::wstring` text, +0xE8 text id (ids below 0x4BE index the language string table at 0x143329E80), +0xF8 glyph block.
- Position: translation of the current world matrix (`GetWorldMatrix` 0x140756C90) in clip space, -1..1, y up.
- The draw colour (0x140756CD0) is white even for the yellow selected menu item, so focus has to come from elsewhere (item 4).
- Button and currency icons are private-use characters (U+E000-U+F8FF). Speech names known keys and DP and drops the rest (see item 4 notes).

## 4. Menus
- [x] Find the cursor/focus state for menus, so the mod speaks only the selected item (`src/menu.cpp`)
- [x] Title screen, main menu, settings (rows, values, volume levels)
- [x] Campaign: series select, duel list, story dialogue
- [x] Deck editor: sections ("Extra, 0 cards"), trunk, Related Cards (Tab), trunk (2), scrolling and re-sorting (X: "Sort by Card Frame, <card>"), the full card view (Y), the filter panel (Space: categories, their options, "Selected" / "Not selected"), and the stats bar (R: "Deck: 23 Effect, 12 Fusion, 15 Spell, 2 Trap, 3 Link")
- [x] Cards: moving the cursor over a card reads its name, "New" and "N of M"; I reads the full info (summary, text, "3 copies"). I is not bound by the game
- [x] Shop: packs read name, price, "N of M cards, N of M copies"; R reads the player's DP; the purchase prompt, "Press to unwrap!" with the new DP, and each pulled card (name, "New", "3 of 8", summary, text) as the arrows move through the pack
- [x] Dialog boxes and confirmation prompts

Notes (how menu speech works, all in `src/menu.cpp`):
- Focus: every widget derives from `RIX::widget_Base`; vtable slot 1 is `SetFocused(widget, bool)` (flag at +0x08). Overrides call the base, so one hook on the base sees all widget kinds. The base is found through RTTI, not a pattern: it is 4 bytes long and not unique.
- A widget's text: walk its scene node (`widget+0x10`, `DFX::TBase`): children `std::set` at +0x28, content at +0xB8. Text layers are content whose vtable is `TLayerText`'s.
- On screen: the draws of every DFX layer class are hooked (`TLayerText`, `TLayerAnimoo`, `TLayerCustom`, `TLayerFrame`, `TLayerFrame2`, `TLayerTexture`), so "something in the widget was drawn after the focus change" separates shown from hidden. The game builds screens off-screen at startup and sets focus there.
- Order: an announcer thread speaks the item (interrupting), then the description queued behind it. Focus events within 30 ms are one announcement; containers of a focused entry and toolbars (`*Bar` classes) are dropped.
- Descriptions: the help bar's setter (`widget_Help` 0x14089F6E0, layer at +0x58), plus any text layer that changes within 500 ms of a focus change; positions where descriptions appear are remembered, so a screen's first description is caught too. Text equal to a recent item (submenu headers) is ignored.
- Screens without focus widgets (Settings, series select): a text layer switching to the focus colour (`MenuColor(true)` 0x140869E80, colour id at layer+0xA0) announces its row.
- Popups: `widget_Base::SetEnabled` (slot 3) sets bit 0x8 of the element flags (+0x88); a class named `*Dialog*`/`*Window*` going hidden to shown is read whole.
- Values: Left/Right (seen by the keyboard hook) then a text change = a value change. Volume bars: `SetVolume(channel, level)` 0x14086BE60.
- Cards: `widget_Card` card id at +0x100 (int16); name from the game's lookup 0x14076D0F0, text from 0x14076D070, summary from `CARD_Prop.bin` read out of YGO_2020.dat (`src/cards.cpp`, test `tests/cards_test.cpp`). A card item's tooltip keeps only the help bar's description (the details panel repeats the card). When several cards take focus in one frame (switching section), the first is spoken.
- Card position: cards inside a `widget_CardDisplay` grid (pack contents, the trunk) say "N of M" from the grid's card array (+0xE8/+0xF0, 0x110 each), card list (+0x210/+0x218, 12 bytes each) and cursor (+0x1B0). Grids are remembered when they take focus and class-checked before use.
- New: the game's own check, vtable slot 1 of the collection object in global 0x14332BD20 (found from SetCard's call). It means "not looked at yet", like the game's NEW mark.
- Shop packs (`widget_PackWrapper`): the two fractions are labelled "cards" (different cards collected) and "copies" (of three each). A focus that starts with no text waits 150 ms for its labels, unless another focus in the batch already had text.
- Key prompts (text opening with an icon) are kept out of values, descriptions and live text, but count as labels of the focused widget (the pack fractions open with icons).
- Story and messages: text changing with no focus change is spoken if the batch has prose (3+ words).

- Scrolling lists reuse row labels. A label whose new text another label showed a moment ago has "moved" (text.cpp): never a description or news. Two or more moving within 50 ms is a scroll, and the row coming into view is dropped too. If the focused row's own label changes with no focus change, the cursor stayed while the list scrolled: that label is the new item (after a game key since the last focus change, or 500 ms after it; fast presses used to be dropped).
- Live text drawn within 150 ms of a focus change is a screen appearing, not news, and is dropped, unless that focus said nothing (its row still off screen, as on the Duel Result screen).
- New text at a new place within 500 ms of a focus change counts as its description when it reads like one (3+ words or several lines); short new labels are a screen being built. A focus with no text of its own (a picture) speaks its description as the item.
- Live text: a batch with prose (3+ words) is spoken and its layers remembered; later changes on those layers (short story lines, the speaker's name) are spoken at any length. A batch of more than 3 labels is a screen appearing: only the focused item and sentences are kept.
- Highlights are a fallback: dropped when a widget focus is in the same batch, and when two or more rows are highlighted at once (the duel HUD draws LP in the focus colour).
- Icons and widths: U+E14F (DP icon) is spoken as "DP"; full-width characters (duel LP "８０００") become ASCII.
- Hotkeys R (name and DP) and H (both duelists' LP) read from drawn text: DP is the text with the DP icon; LP is the number on each "LP" label's row, the player's on the bottom half.
- Game bug fixed (`src/dllmain.cpp`, `FixExitHang`): quitting froze the game. The worker thread re-set the constant window title (0x1408CB760 → `SetWindowTextW`) while the main thread had stopped pumping messages and was waiting for it. Calls from a thread other than the window's are now skipped.
- A widget refocused within 100 ms of losing focus is ignored: Video Settings unfocuses and refocuses Apply/Back every frame (was ~17,000 focus events and constant "Apply" speech).

- Key icons are spoken as key names (`KeyName` in text.cpp, mapped from the Controls screen): digits U+E169.., letters U+E173.., plus arrows, Enter, Backspace, Esc, Space, Tab, Shifts, Ctrls, PgUp/PgDn, Home, End, Pause, mouse and pad buttons. Adjacent icons read "Enter or A".
- The card under the cursor can change without a focus event (trunk scrolling past its visible rows, re-sorting): `SetCard` (0x140882D80) on the focused card widget announces the new card.
- Sort bars: their labels are remembered when they take focus; a change is announced first ("Sort by Level/Rank, <card>").
- Filter options (`widget_TrunkFilterMaskOption`): check-mark sprite at +0xA0 (+0x24 index), 0x150 = selected; `RefreshCheck` (0x1408BAC10) announces toggles. Helper buttons (`widget_ButtonHelp`) are dropped like toolbars.
- Deck stats (`widget_DeckStats::Refresh` 0x1408973A0): 10 counts from its source's vtable slot 2, in the bar's colour order (Normal, Effect, Ritual, Fusion, Synchro, Spell, Trap, Xyz, Pendulum, Link; checked against Best Hero: 40 main + 15 extra).
- The campaign's empty duel slot reads "Not available".

## 5. Duels
- [x] Speak phase changes, turn changes and LP changes ("Turn 3, your turn. Your Draw Phase", "You lose 1600 LP, now 6400")
- [x] Speak the card under the cursor: name, position, place, current ATK/DEF ("Elemental HERO Knospe, attack, your monster zone 4, ATK 700 (original 600), DEF 900 (original 1000)", "Polymerization, your hand, 3 of 5"), empty zones and piles ("Your Deck, 35 cards", "Your Graveyard, 2 cards, top Garoozis"); I reads the full info
- [x] Hotkeys to review the board: F field, U hand, G graveyard, V banished, E Extra Deck, T turn/phase/deck counts, H LP; Alt with F/U/G/V/E for the opponent
- [x] Speak the action menu (summon, set, activate, attack), zone and target selection, phase selection, effect choices, Yes/No and Ok prompts, card lists to pick from
- [x] Speak chain resolution, effect activations and opponent actions (draws, sets, summons, activations, attacks, moves between zones, flips, position changes, targets, coin/dice, shuffles)
- [x] Speak duel results ("You lose the duel", the result banner, the report window's rewards)

Notes (all in `src/duel.cpp` unless said):
- Events: hook on `Draw_DuelAnimationFromId(anim, a2, a3, a4)` 0x1407C1450 (ids in Arefu's `docs/DuelAnimations.md`). Spoken queued. Locations are packed `side | zone << 1 | index << 6` (bit 0x4000/0x8000 flags; zone 18 = nowhere). What the log showed where Arefu's table is wrong or silent:
  - 16 LP change comes twice (a4 0x101 then 0x100) and before the LP is applied: LP are tracked from 15 (LP set) plus each change.
  - 26 move: kind in a2 >> 16, 9 = draw, 0xB = an effect adding from the Deck. To zone 18 = a token leaving the field.
  - 28 is a face or position change, not CardReturn: a3 1 = flipped face-up (the record still says face-down), 0 = position changed (the record has it).
  - 35 activate: a3 is the card id. 57 normal summon: a3 is not; the card is read from its zone.
  - 53 chain: length in the low 16 bits of a4. 55 message: (4, 1, 0) is sent constantly; any other kind or a4 means a box follows (menu.cpp `MenuExpectMessage`: its line becomes the box's description). 56: a list or option to pick from opens. 6: turn change (its banner isn't read).
- Board: `Duel::PlayerState` 0x143497C40, 0xD94 per side. +0x4C: the 13 field zones in zone order, 0x18 each: +0 id (low 14 bits), +6 bit 0 defense, +7 bit 0 face-up. Piles at +0x19C hand, +0x55C Extra Deck, +0x7B4 GY, +0xA0C banished; counts at +0x0C. The opponent's hand, Extra Deck, Deck and face-down cards are never named. Extra Monster Zone records are mapped by zone number (5, 6) but not seen in a duel yet.
- Cursor: 0x14278EE78 (packed location); 0x140773660 runs after every cursor move (and also with the cursor staying put). Spoken only within 1 s of a key the game saw after "Duel start" (the mod's hotkeys don't count); the engine's own pointing is covered by the events.
- ATK/DEF on the field: `Calc_EffectiveAtkDef_Modifiers(player, zone, out, 0, 0)` 0x1400307C0 (found through `Get_EffectiveAtk` 0x140039B30's call), out +4 ATK, +8 DEF, with equips, field spells and effects applied. "(original N)" when it differs from the printed value. Verified with Knospe's own effect (+100 ATK, -100 DEF).
- Materials: 43/46/49/82/87 Mark (picked) and 45/48/51/84/89 Play (used), (side, zone, a4); a4's low 16 bits are the card's instance = bit 14 + (bits 23-30) x 2 of its pile or zone word (the engine's lookup 0x140044480). Hand materials are found by instance; field ones say their zone ("Ancient Gear in the opponent's monster zone 3 selected as Tribute"). Marks are re-sent during a summon: each is said once until a Clear or Play.
- Details panel: `0x1408866B0(widget, info)`, info +6 id, +8 location. Speaks only list entries from the Deck, Extra Deck, Graveyard or banishment after a key or as the list opens (anim 56) ("Baby Dragon, your Deck"), and cards with no place (tokens being made). Field and hand cards are the cursor's and the events'. Sets what I reads.
- Duel prompts (popups and plain text) get the cursor's place appended when they are about it: an empty field zone for "Select position for X", the card the prompt names ("Select [Elemental HERO Clayman] from your hand"), "to activate", or a target ("Select an attack target monster. Ancient Gear, attack, ...").
- Effect choices (`DUEL_DIALOG_SELECT_EFFECT`, setup 0x14077B280): can't-use flags at +0x2F8 + i, descriptions (std::wstring) at +0x300 + i * 0x20; spoken descriptions get "Choice 2 of 3: " or "not available" (`DuelAnnotate`, applied in menu.cpp's `SpeakW`). Not yet heard in game: the dialog's tabs were read before this was added.
- In a duel (menu.cpp): a highlight reads only its own label (Yes/No share a row); the card panels (left edge, and the right one during targets and materials) and the HUD corners (names, LP counting down, turn counter) aren't live text or descriptions; a relabelled row that repeats an announcement is dropped.
- Card lists open hidden `FilterWindow_t`/`SortWindow_t` that the popup hook saw re-shown every frame; they buried other focus (the list's OK button wasn't read). A widget re-shown within 1 s isn't a new popup.
- Details-panel suppression (`MenuSuppressLive`) drops only text that starts with the card's name, not text mentioning it (a tutorial line naming [Polymerization] was lost).
- Checked in GX campaign duel 1 (Fusion Summon tutorial vs. Dr. Crowler), start to finish: tutorial lines, two Fusion Summons, Fusion Recovery's Graveyard targets, Skyscraper, both turns of Crowler's, the win and the result screen.
- Duel state: `ExGameDuel::OnDispatch` (vtable slot 5) runs every frame of a duel; a duel whose dispatch stopped for 1 s is over (Pause > Exit Game sends no result). The duel screen's destructor never runs between duels.
- Janken: hand at +0xE04 of the Janken object in state 5 (update 0x1407B3CB0).
- In a duel, Enter on your Deck opens a menu whose first item is Surrender.

## 6. Release
- [x] Hotkey help and README for players: F1 reads the hotkeys; `README.html` (install, hotkeys, what is read, limitations) ships in the zip
- [x] Re-check hooks after each game update: every `FindInGame` / `FindVTable` miss is counted (`GameMisses()`, `src/scan.cpp`), and a non-zero count is spoken after "mod loaded" as a warning that the game probably updated
- [x] Package: proxy DLL, `prism.dll`, licenses (`scripts/package_release.ps1`, built and published by `.github/workflows/release.yml` on a `v*` tag; `/release` skill drives it)

Notes:
- After a game update: start the game, read `ygol-access.log` for "expected 1" and "RTTI: no" lines, find the moved code, check new patterns with `tools/patcheck.py`, then release.
- `DUEL_DIALOG_SELECT_EFFECT` is a struct (`.?AU`) in this build; the `.?AV` name is only the fallback, so a normal start has no misses.
