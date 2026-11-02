#pragma once
#include <cstdint>
#include <string>

// A card's kind, attribute, type, level and ATK/DEF in words, from its CARD_Prop.bin record
// (format: tools/cards.py). E.g. "Effect monster, DARK Fiend, Level 1, ATK 300, DEF 200" or "Quick-Play Spell".
std::wstring DescribeProp(uint32_t first, uint32_t second);

// The same for a card id, reading CARD_Prop.bin from the game's YGO_2020.dat on first use. Empty if unknown.
std::wstring CardSummary(int16_t cardId);
