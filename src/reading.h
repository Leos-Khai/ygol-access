#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Splits card info into chunks for reading one at a time. Explicit line breaks are boundaries; a line longer than
// softLimit is split at sentence ends, then at the last space before the limit.
std::vector<std::wstring> SplitForReading(std::wstring_view text, size_t softLimit = 120);

// Steps a chunk index by direction (-1 previous, +1 next), wrapping at both ends. A count of 0 returns 0.
size_t StepReadingChunk(size_t index, size_t count, int direction);
