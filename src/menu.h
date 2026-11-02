#pragma once
#include <cstdint>
#include <string>

// The game's card name and text for a card id, in the game's language (empty if unknown).
std::wstring GameCardName(int16_t id);
std::wstring GameCardText(int16_t id);

// Speaks menu focus changes for every widget kind, followed by the screen's description/tooltip.
// Needs TextHooksInstall first (uses its text layer lookups).
bool MenuHooksInstall();

// Tells the menu code Left or Right was pressed: text that changes next is a value change and is spoken.
void MenuValueKeyPressed();

// Tells the menu code any key was pressed (the game's or the mod's). Focus changes long after the last key were
// not the user's doing (the opponent choosing), and flicker between items is not repeated.
// gameKey: the game sees the key (it isn't one of the mod's hotkeys).
void MenuAnyKeyPressed(bool gameKey);
// GetTickCount64 of the last key press the game saw.
unsigned long long MenuLastGameKeyAt();

// Something else (the duel code) just said this; screen text repeating it (a phase banner) is not read again.
void MenuSpokeElsewhere(const std::wstring& text);

// The duel is opening a message box: the new line drawn with its button is read after the button.
void MenuExpectMessage();

// Speaks the focused card's full info (summary, text, copies): the I key.
void MenuSpeakCardInfo();

// Sets what I reads (the duel's card under the cursor).
void MenuSetCardInfo(const std::wstring& info);

// Screen text starting with this (a card details panel opens with the card just announced) is not read as news
// for a moment.
void MenuSuppressLive(const std::wstring& containing);

// Speaks the deck editor's stats bar ("Deck: 3 Effect, 12 Spell") if it is on screen; false if it isn't.
bool MenuSpeakDeckStats();
