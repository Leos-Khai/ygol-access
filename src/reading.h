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

// Builds the card info used by I and the reading buffer. The name, monster type/attribute line, level/rank/link
// and stats line, card text and copy count are separated by line breaks, so each becomes its own chunk where
// possible. Text already cleaned by CleanForSpeech should be passed in.
std::wstring BuildCardInfo(std::wstring_view name, std::wstring_view summary, std::wstring_view text,
                           std::wstring_view copies = {});
