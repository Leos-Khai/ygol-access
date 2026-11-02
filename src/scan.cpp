#include "scan.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <vector>

void Log(const char* fmt, ...);

// -1 is a wildcard. Empty result means the pattern is malformed.
static std::vector<int> Parse(const char* pattern) {
    std::vector<int> bytes;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            ++p;
        } else if (*p == '?') {
            bytes.push_back(-1);
            p += p[1] == '?' ? 2 : 1;
        } else {
            char* end;
            long v = strtol(p, &end, 16);
            if (end - p != 2) return {};
            bytes.push_back((int)v);
            p = end;
        }
    }
    return bytes;
}

static const uint8_t* Next(const uint8_t* begin, const uint8_t* end, const std::vector<int>& bytes) {
    if (bytes.empty() || end - begin < (ptrdiff_t)bytes.size()) return nullptr;
    for (const uint8_t* at = begin; at <= end - bytes.size(); ++at) {
        size_t i = 0;
        while (i < bytes.size() && (bytes[i] < 0 || at[i] == bytes[i])) ++i;
        if (i == bytes.size()) return at;
    }
    return nullptr;
}

const uint8_t* FindPattern(const uint8_t* begin, const uint8_t* end, const char* pattern) {
    return Next(begin, end, Parse(pattern));
}

size_t CountPattern(const uint8_t* begin, const uint8_t* end, const char* pattern) {
    std::vector<int> bytes = Parse(pattern);
    size_t count = 0;
    for (const uint8_t* at = Next(begin, end, bytes); at; at = Next(at + 1, end, bytes)) ++count;
    return count;
}

bool GameSection(const char* name, uint8_t** begin, uint8_t** end) {
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (strncmp((const char*)section->Name, name, IMAGE_SIZEOF_SHORT_NAME) != 0) continue;
        *begin = base + section->VirtualAddress;
        *end = *begin + section->Misc.VirtualSize;
        return true;
    }
    Log("No %s section in YuGiOh.exe", name);
    return false;
}

static int g_misses;
void NoteGameMiss() { ++g_misses; }
int GameMisses() { return g_misses; }

uint8_t* FindInGame(const char* name, const char* pattern) {
    uint8_t *begin, *end;
    if (!GameSection(".text", &begin, &end)) {
        NoteGameMiss();
        return nullptr;
    }
    size_t count = CountPattern(begin, end, pattern);
    if (count != 1) {
        Log("Pattern %s: %zu matches, expected 1", name, count);
        NoteGameMiss();
        return nullptr;
    }
    auto* found = (uint8_t*)FindPattern(begin, end, pattern);
    Log("Pattern %s at YuGiOh.exe+0x%llX", name, (unsigned long long)(found - (uint8_t*)GetModuleHandleW(nullptr)));
    return found;
}
