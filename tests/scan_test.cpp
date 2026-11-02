#undef NDEBUG  // keep asserts in Release builds
#include "../src/scan.h"

#include <cassert>
#include <cstdio>

void Log(const char*, ...) {}

int main() {
    const uint8_t data[] = {0x48, 0x8B, 0x05, 0x10, 0x48, 0x8B, 0x0D, 0x10, 0xCC};
    const uint8_t* end = data + sizeof data;

    assert(FindPattern(data, end, "48 8B 05") == data);
    assert(FindPattern(data, end, "8B 0D") == data + 5);
    assert(FindPattern(data, end, "48 8B ?? 10") == data);
    assert(FindPattern(data, end, "48 8B ? 10 48") == data);
    assert(FindPattern(data, end, "10 CC") == data + 7);
    assert(FindPattern(data, end, "CC 00") == nullptr);   // runs past the end
    assert(FindPattern(data, end, "4") == nullptr);       // malformed
    assert(FindPattern(data, end, "") == nullptr);
    assert(CountPattern(data, end, "48 8B ?? 10") == 2);
    assert(CountPattern(data, end, "48 8B 0D") == 1);
    assert(CountPattern(data, end, "AA") == 0);
    puts("scan_test passed");
    return 0;
}
