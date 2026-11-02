#pragma once
#include <cstddef>
#include <cstdint>

// Byte patterns are hex bytes separated by spaces, with "?" or "??" as a wildcard: "48 8B ?? 05".

// Returns the first match in [begin, end), or nullptr. Also returns nullptr for a malformed pattern.
const uint8_t* FindPattern(const uint8_t* begin, const uint8_t* end, const char* pattern);

// Counts matches in [begin, end).
size_t CountPattern(const uint8_t* begin, const uint8_t* end, const char* pattern);

// Gets the address range of a YuGiOh.exe section such as ".text", ".rdata" or ".data".
bool GameSection(const char* name, uint8_t** begin, uint8_t** end);

// Finds a pattern in YuGiOh.exe's .text section. Returns nullptr (and logs) unless there is
// exactly one match, so a game update can't silently point a hook at the wrong code.
uint8_t* FindInGame(const char* name, const char* pattern);

// Game code or classes the mod looked for and didn't find (FindInGame, FindVTable): non-zero after a game
// update moved something, so the player can be told.
void NoteGameMiss();
int GameMisses();
