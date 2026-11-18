#include "reading.h"

namespace {

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

bool IsQuote(wchar_t c) {
    return c == L'"' || c == L'\u201C' || c == L'\u201D';
}

std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
    return text;
}

void AddChunk(std::vector<std::wstring>& out, std::wstring_view text) {
    text = Trim(text);
    if (!text.empty()) out.emplace_back(text.data(), text.size());
}

// After a sentence ending followed by whitespace, or at the end of the text. Punctuation inside a quoted card
// name does not end a sentence.
size_t SentenceEnd(std::wstring_view text, size_t from) {
    bool quoted = false;
    for (size_t i = from; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (IsQuote(c)) {
            quoted = !quoted;
            continue;
        }
        if (quoted || (c != L'.' && c != L'!' && c != L'?')) continue;
        if (i + 1 == text.size() || IsSpace(text[i + 1])) return i + 1;
    }
    return text.size();
}

// Keeps a code-unit cut from landing in the middle of a surrogate pair.
size_t SafeCut(std::wstring_view text, size_t cut) {
    if (cut > 0 && cut < text.size() && text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF &&
        text[cut] >= 0xDC00 && text[cut] <= 0xDFFF)
        return cut - 1;
    return cut;
}

// The last space at or before `end`, unless that space is inside a quoted card name. Then use the last space
// before the opening quote, or if there is none, extend past the closing quote so the name stays whole.
size_t QuoteSafeCut(std::wstring_view text, size_t start, size_t end) {
    size_t space = text.rfind(L' ', end);
    if (space == std::wstring_view::npos || space <= start) return end;

    bool quoted = false;
    size_t opening = std::wstring_view::npos;
    for (size_t i = 0; i < space; ++i) {
        if (!IsQuote(text[i])) continue;
        quoted = !quoted;
        if (quoted) opening = i;
    }
    if (!quoted) return space;

    size_t before = text.rfind(L' ', opening);
    if (before != std::wstring_view::npos && before > start) return before;

    size_t close = opening + 1;
    while (close < text.size() && !IsQuote(text[close])) ++close;
    if (close < text.size()) ++close;  // past the closing quote
    size_t after = text.find(L' ', close);
    return after == std::wstring_view::npos ? text.size() : after;
}

void AddLong(std::vector<std::wstring>& out, std::wstring_view text, size_t softLimit) {
    size_t start = 0;
    while (text.size() - start > softLimit) {
        size_t end = start + softLimit;
        size_t cut = QuoteSafeCut(text, start, end);
        cut = SafeCut(text, cut);
        if (cut <= start) cut = end;  // a limit that cannot hold one character
        AddChunk(out, text.substr(start, cut - start));
        start = cut;
        while (start < text.size() && IsSpace(text[start])) ++start;
    }
    AddChunk(out, text.substr(start));
}

void AddLine(std::vector<std::wstring>& out, std::wstring_view line, size_t softLimit) {
    line = Trim(line);
    if (line.empty()) return;
    if (line.size() <= softLimit) {
        out.emplace_back(line.data(), line.size());
        return;
    }
    // A long line: one chunk per sentence. A sentence still over the limit is split at spaces.
    size_t start = 0;
    while (start < line.size()) {
        while (start < line.size() && IsSpace(line[start])) ++start;
        if (start >= line.size()) break;
        size_t end = SentenceEnd(line, start);
        AddLong(out, line.substr(start, end - start), softLimit);
        start = end;
    }
}

}  // namespace

std::vector<std::wstring> SplitForReading(std::wstring_view text, size_t softLimit) {
    if (softLimit == 0) softLimit = 120;
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring_view::npos) end = text.size();
        AddLine(out, text.substr(start, end - start), softLimit);
        if (end == text.size()) break;
        start = end + 1;
    }
    return out;
}

size_t StepReadingChunk(size_t index, size_t count, int direction) {
    if (count == 0) return 0;
    index %= count;
    if (direction < 0) return index == 0 ? count - 1 : index - 1;
    if (direction > 0) return index + 1 == count ? 0 : index + 1;
    return index;
}
