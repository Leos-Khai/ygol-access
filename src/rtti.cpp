#include "rtti.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "scan.h"

void Log(const char* fmt, ...);

// x64 RTTI: vtable[-1] points to this. All "pointers" are RVAs from the image base.
struct CompleteObjectLocator {
    uint32_t signature;  // 1 on x64
    uint32_t offset;     // where this vtable sits in the complete object; 0 for the primary vtable
    uint32_t cdOffset;
    int32_t typeDescriptor;  // -> { void* vtable; void* spare; char name[]; }
    int32_t classDescriptor;
    int32_t self;  // RVA of this locator
};

static uint8_t* Base() { return (uint8_t*)GetModuleHandleW(nullptr); }

static bool In(const void* p, const uint8_t* begin, const uint8_t* end, size_t size) {
    return (const uint8_t*)p >= begin && (const uint8_t*)p + size <= end;
}

void** FindVTable(const char* decoratedName) {
    uint8_t *rdata, *rdataEnd, *data, *dataEnd;
    if (!GameSection(".rdata", &rdata, &rdataEnd) || !GameSection(".data", &data, &dataEnd)) return nullptr;
    uint8_t* base = Base();

    // Type descriptors live in .data: the name follows two pointers.
    size_t len = strlen(decoratedName) + 1;
    const uint8_t* name = nullptr;
    for (uint8_t* p = data + 16; p + len <= dataEnd; ++p) {
        if (memcmp(p, decoratedName, len) == 0) {
            name = p;
            break;
        }
    }
    if (!name) {
        Log("RTTI: no type %s", decoratedName);
        return nullptr;
    }
    int32_t typeRva = (int32_t)(name - 16 - base);

    for (uint8_t* p = rdata; p + sizeof(CompleteObjectLocator) <= rdataEnd; p += 4) {
        auto* col = (CompleteObjectLocator*)p;
        if (col->signature != 1 || col->offset != 0 || col->typeDescriptor != typeRva || col->self != (int32_t)(p - base)) continue;
        for (uint8_t* q = rdata; q + 16 <= rdataEnd; q += 8) {
            if (*(uint8_t**)q == p) return (void**)(q + 8);
        }
    }
    Log("RTTI: no vtable for %s", decoratedName);
    return nullptr;
}

const char* ClassName(const void* object) {
    static uint8_t *rdata, *rdataEnd, *data, *dataEnd;
    static bool ok = GameSection(".rdata", &rdata, &rdataEnd) && GameSection(".data", &data, &dataEnd);
    if (!ok || !object) return nullptr;

    auto* vtable = *(uint8_t* const*)object;
    if (!In(vtable - 8, rdata, rdataEnd, 8)) return nullptr;
    auto* col = *(CompleteObjectLocator* const*)(vtable - 8);
    if (!In(col, rdata, rdataEnd, sizeof *col) || col->signature != 1) return nullptr;
    const uint8_t* type = Base() + col->typeDescriptor;
    if (!In(type, data, dataEnd, 17)) return nullptr;
    return (const char*)type + 16;
}
