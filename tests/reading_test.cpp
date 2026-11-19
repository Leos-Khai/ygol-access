#undef NDEBUG  // keep asserts in Release builds
#include "../src/reading.h"

#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    using Chunks = std::vector<std::wstring>;

    assert(SplitForReading(L"one\ntwo\n\nthree") == (Chunks{L"one", L"two", L"three"}));
    assert(SplitForReading(L"  one  \r\n two \r\n") == (Chunks{L"one", L"two"}));
    assert(SplitForReading(L"\n \r\n\t\n") == Chunks{});

    // A long line splits at sentence ends.
    assert(SplitForReading(L"First sentence. Second sentence. Third.", 20) ==
           (Chunks{L"First sentence.", L"Second sentence.", L"Third."}));

    // A sentence still over the limit splits at spaces.
    assert(SplitForReading(L"aaaa bbbb cccc dddd eeee", 12) == (Chunks{L"aaaa bbbb", L"cccc dddd", L"eeee"}));

    // A quoted card name is kept whole: cut before the opening quote when possible.
    assert(SplitForReading(L"aaaa bbbb \"quoted card name\" cccc dddd", 16) ==
           (Chunks{L"aaaa bbbb", L"\"quoted card name\"", L"cccc dddd"}));

    // No space before the quote: extend past the closing quote instead.
    assert(SplitForReading(L"\"The Winged Dragon of Ra - Sphere Mode\" from your hand", 20) ==
           (Chunks{L"\"The Winged Dragon of Ra - Sphere Mode\"", L"from your hand"}));

    // The real long-sentence case: the card name stays in one chunk.
    Chunks ra = SplitForReading(
        L"Once per turn, during the End Phase: Send this card to the GY, and if you do, Special Summon 1 "
        L"\"The Winged Dragon of Ra - Sphere Mode\" from your hand.",
        120);
    assert(ra.size() == 2);
    assert(ra[1].starts_with(L"\"The Winged Dragon of Ra - Sphere Mode\""));

    // A full stop inside a quoted name is not a sentence boundary.
    assert(SplitForReading(L"\"Stop. Now.\" Next.", 10) == (Chunks{L"\"Stop. Now.\"", L"Next."}));

    // Line breaks still win inside a longer text.
    assert(SplitForReading(L"short\nA long enough sentence here. Another sentence.", 20) ==
           (Chunks{L"short", L"A long enough", L"sentence here.", L"Another sentence."}));

    // Card info is built with one section per line: name, attributes, stats, text, copies.
    assert(BuildCardInfo(L"Archfiend Commander", L"Effect monster, DARK Fiend, Level 6, ATK 2500, DEF 1200",
                         L"If you control an \"Archfiend\" card.", L"1 copies") ==
           L"Archfiend Commander.\nEffect monster, DARK Fiend\nLevel 6, ATK 2500, DEF 1200.\n"
           L"If you control an \"Archfiend\" card.\n1 copies");

    Chunks archfiend = SplitForReading(BuildCardInfo(
        L"Archfiend Commander", L"Effect monster, DARK Fiend, Level 6, ATK 2500, DEF 1200",
        L"If you control an \"Archfiend\" card.", L"1 copies"));
    assert(archfiend == (Chunks{L"Archfiend Commander.", L"Effect monster, DARK Fiend",
                                L"Level 6, ATK 2500, DEF 1200.", L"If you control an \"Archfiend\" card.",
                                L"1 copies"}));

    // A dotted card name stays in one chunk.
    Chunks bes = SplitForReading(BuildCardInfo(
        L"B.E.S. Tetran", L"Effect monster, DARK Machine, Level 4, ATK 1800, DEF 1000",
        L"When this card is Normal Summoned, put 3 counters on it.", L"1 copies"));
    assert(bes[0] == L"B.E.S. Tetran.");

    Chunks dd = SplitForReading(BuildCardInfo(
        L"D.D. Assailant", L"Effect monster, EARTH Warrior, Level 4, ATK 1700, DEF 1600",
        L"After damage calculation, when this card is destroyed by battle: banish this card.", L"1 copies"));
    assert(dd[0] == L"D.D. Assailant.");

    // The material line no longer joins the name, attributes and stats into one chunk.
    Chunks skull = SplitForReading(BuildCardInfo(
        L"B. Skull Dragon", L"Fusion monster, DARK Dragon, Level 9, ATK 3200, DEF 2500",
        L"\"Summoned Skull\" + \"Red-Eyes B. Dragon\"", L"1 copies"));
    assert(skull == (Chunks{L"B. Skull Dragon.", L"Fusion monster, DARK Dragon",
                            L"Level 9, ATK 3200, DEF 2500.",
                            L"\"Summoned Skull\" + \"Red-Eyes B. Dragon\".", L"1 copies"}));

    // Link ratings split from the type line like Level and Rank.
    Chunks link = SplitForReading(BuildCardInfo(
        L"Link Card", L"Link Effect monster, LIGHT Cyberse, Link 3, ATK 2300, arrows top", L"", L""));
    assert(link == (Chunks{L"Link Card.", L"Link Effect monster, LIGHT Cyberse",
                           L"Link 3, ATK 2300, arrows top"}));

    // Copies are always their own final chunk.
    Chunks banner = SplitForReading(BuildCardInfo(
        L"Banner of Courage", L"Continuous Spell",
        L"All monsters you control gain 200 ATK during your Battle Phase only.", L"0 copies"));
    assert(banner.back() == L"0 copies");

    // Wrapping in both directions.
    assert(StepReadingChunk(0, 3, -1) == 2);
    assert(StepReadingChunk(2, 3, 1) == 0);
    assert(StepReadingChunk(1, 3, 1) == 2);
    assert(StepReadingChunk(1, 1, -1) == 0);
    assert(StepReadingChunk(0, 0, 1) == 0);

    puts("reading_test passed");
    return 0;
}
