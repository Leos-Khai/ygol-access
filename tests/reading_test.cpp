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

    // Wrapping in both directions.
    assert(StepReadingChunk(0, 3, -1) == 2);
    assert(StepReadingChunk(2, 3, 1) == 0);
    assert(StepReadingChunk(1, 3, 1) == 2);
    assert(StepReadingChunk(1, 1, -1) == 0);
    assert(StepReadingChunk(0, 0, 1) == 0);

    puts("reading_test passed");
    return 0;
}
