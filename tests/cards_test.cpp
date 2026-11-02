#undef NDEBUG  // keep asserts in Release builds
#include "../src/cards.h"

#include <cassert>
#include <cstdio>

void Log(const char*, ...) {}

int main() {
    // Records from CARD_Prop.bin (checked against tools/cards.py output).
    assert(DescribeProp(0x0A078F3D, 0x080C0915) == L"Token monster, DARK Fiend, Level 1, ATK 300, DEF 200");  // Kuriboh Token
    assert(DescribeProp(0x05028F3C, 0x08280A95) == L"Token monster, EARTH Insect, Level 1, ATK 100, DEF 100");  // Insect Monster Token
    // Spell: kind 13, icon 5 (Quick-Play).
    assert(DescribeProp(0, (13u << 1) | (5u << 15)) == L"Quick-Play Spell");
    // Link: kind 43, LIGHT, Link 3, Cyberse, ATK 2300, arrows top + bottom-left + bottom-right (bits 1, 5, 7).
    uint32_t first = (230u << 14) | (0xA2u << 23);
    uint32_t second = (43u << 1) | (1u << 7) | (3u << 11) | (23u << 18) | (3u << 27);
    assert(DescribeProp(first, second) == L"Link Effect monster, LIGHT Cyberse, Link 3, ATK 2300, arrows top bottom-left bottom-right");
    puts("cards_test passed");
    return 0;
}
