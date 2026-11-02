#include "text.h"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rtti.h"
#include "scan.h"
#include "speech.h"

// DFX::TLayerText layout (YuGiOh.exe, see CHECKLIST.md item 3):
//   +0xA0 colour id (menus set it to the focus colour on the selected label)
//   +0xC8 std::wstring text (MSVC: inline buffer or pointer at +0xC8, size +0xD8, capacity +0xE0)
//   +0xE8 text id, +0xF8 laid-out glyph block
constexpr size_t kColorOffset = 0xA0, kTextOffset = 0xC8, kSizeOffset = 0xD8, kCapacityOffset = 0xE0;

constexpr wchar_t kDuelPointsIcon = 0xE14F;  // drawn after DP amounts ("75652" + icon)

// Key and button icons (private-use characters) as words. Mapped from the Help & Options > Controls screen,
// whose rows show each function's keys: digits and letters are consecutive, the rest are listed.
static std::wstring KeyName(wchar_t c) {
    if (c >= 0xE169 && c <= 0xE172) return std::wstring(1, L'0' + (c - 0xE169));
    if (c >= 0xE173 && c <= 0xE18C) return std::wstring(1, L'A' + (c - 0xE173));
    switch (c) {
    case 0xE1C7: return L"Up";
    case 0xE1A4: return L"Down";
    case 0xE1AE: return L"Left";
    case 0xE1BF: return L"Right";
    case 0xE1B3: return L"Enter";
    case 0xE19E: return L"Backspace";
    case 0xE1A8: return L"Escape";
    case 0xE1C4: return L"Space";
    case 0xE1B7: return L"Pause";
    case 0xE1C5: return L"Tab";
    case 0xE1AF: return L"Left Shift";
    case 0xE1C0: return L"Right Shift";
    case 0xE1AD: return L"Left Ctrl";
    case 0xE1BE: return L"Right Ctrl";
    case 0xE1B6: return L"Page Up";
    case 0xE1B5: return L"Page Down";
    case 0xE1A9: return L"Home";
    case 0xE1A5: return L"End";
    case 0xE1C9: return L"Left click";
    case 0xE1CA: return L"Middle click";
    case 0xE1CB: return L"Right click";
    case 0xE1CC: return L"Mouse wheel";
    case 0xE001: return L"Pad A";
    case 0xE002: return L"Pad B";
    case 0xE003: return L"Pad X";
    case 0xE004: return L"Pad Y";
    case 0xE005: return L"Pad LB";
    case 0xE006: return L"Pad RB";
    case 0xE007: return L"Pad LT";
    case 0xE008: return L"Pad RT";
    case 0xE00A: return L"Pad Menu";
    case 0xE00B: return L"D-pad";
    case 0xE00D: return L"Right stick";
    default: return L"";
    }
}

// Rows: text within 0.03 clip-space units (about 1.5% of the screen height) vertically.
constexpr float kRowTolerance = 0.03f;

// Text not drawn for this long is treated as gone from the screen.
constexpr ULONGLONG kOnScreenMs = 150;

using DrawFn = void(__fastcall*)(void* layer);
using GetMatrixFn = float*(__fastcall*)(float* out);  // current 4x4 world matrix; translation is clip space

using MenuColorFn = int(__fastcall*)(bool focused);  // the game's colour id for a normal or focused label

void** g_textLayerVTable;
TextChangedFn g_onTextChanged;
HighlightFn g_onHighlight;
static DrawFn g_draw;
static GetMatrixFn g_getMatrix;
static MenuColorFn g_menuColor;

struct Drawn {
    std::wstring text;
    float x = 0, y = 0;
    int color = -1;
    ULONGLONG lastDrawn = 0;
};
static std::mutex g_mutex;
static std::unordered_map<const void*, Drawn> g_drawn;  // keyed by layer; pointers are never dereferenced later
static std::vector<std::pair<std::wstring, ULONGLONG>> g_replaced;  // texts labels just stopped showing

std::string ToUtf8(std::wstring_view w) {
    std::string out(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), (int)out.size(), nullptr, nullptr);
    return out;
}

std::wstring CleanForSpeech(std::wstring_view text) {
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'@') {  // formatting codes: "@4...@0", "@C[...]" colours, "@/...@" italics
            if (i + 1 < text.size() && (iswalnum(text[i + 1]) || text[i + 1] == L'/')) ++i;
            continue;
        }
        if (c == kDuelPointsIcon) {  // the DP currency icon follows the amount
            out += L" DP";
            continue;
        }
        if (c >= 0xE000 && c <= 0xF8FF) {  // other private-use chars are the game's key and decoration icons
            std::wstring key = KeyName(c);
            if (key.empty()) continue;
            bool afterKey = i > 0 && !KeyName(text[i - 1]).empty();
            if (!out.empty() && out.back() != L' ') out += L' ';
            out += (afterKey ? L"or " : L"") + key + L' ';  // "[Enter][A]" -> "Enter or A"
            continue;
        }
        if (c >= 0xFF01 && c <= 0xFF5E) c -= 0xFEE0;  // full-width forms (duel LP "８０００") to plain ASCII
        if (c == 0x3000) c = L' ';                     // ideographic space
        if (c == L'|') c = L'\n';                      // the game's line break (flavour text ends "@|")
        if (c == L' ' && !out.empty() && out.back() == L' ') continue;  // the gap an icon leaves
        out += c;
    }
    size_t first = out.find_first_not_of(L" \r\n\t");
    size_t last = out.find_last_not_of(L" \r\n\t");
    return first == std::wstring::npos ? L"" : out.substr(first, last - first + 1);
}

std::wstring_view LayerText(const void* layer) {
    auto* p = (const uint8_t*)layer;
    size_t size = *(const size_t*)(p + kSizeOffset);
    size_t capacity = *(const size_t*)(p + kCapacityOffset);
    if (size > capacity || size > 4096) return {};  // not a sane string: skip rather than read garbage
    auto* chars = capacity > 7 ? *(const wchar_t* const*)(p + kTextOffset) : (const wchar_t*)(p + kTextOffset);
    return {chars, size};
}

LayerSeen LayerLastDrawn(const void* layer) {
    std::lock_guard lock(g_mutex);
    auto it = g_drawn.find(layer);
    if (it == g_drawn.end()) return {false, 0, 0, 0};
    return {true, GetTickCount64() - it->second.lastDrawn, it->second.x, it->second.y};
}

static std::string ForLog(std::wstring_view text) {
    std::string line = ToUtf8(text);
    std::replace(line.begin(), line.end(), '\n', '|');
    std::replace(line.begin(), line.end(), '\r', ' ');
    return line;
}

static void __fastcall DrawHook(void* layer) {
    g_draw(layer);

    std::wstring_view text = LayerText(layer);
    if (text.empty()) return;
    float m[16];
    g_getMatrix(m);
    ULONGLONG now = GetTickCount64();

    int color = *(const int*)((const uint8_t*)layer + kColorOffset);
    bool changed, firstSeen, highlighted, moved = false;
    {
        std::lock_guard lock(g_mutex);
        auto [it, inserted] = g_drawn.try_emplace(layer);
        Drawn& d = it->second;
        // Not drawn for a second: a new layer at a reused address, or a screen coming back. Either way, new.
        firstSeen = inserted || now - d.lastDrawn > 1000;
        changed = d.text != text;
        if (changed) {
            // Lists reuse row labels when they scroll, so a label's new text was just on another label.
            moved = std::any_of(g_drawn.begin(), g_drawn.end(), [&](const auto& e) {
                return e.first != layer && e.second.text == text && now - e.second.lastDrawn < 200;
            }) || std::any_of(g_replaced.begin(), g_replaced.end(), [&](const auto& r) {
                return r.first == text && now - r.second < 200;
            });
            if (!d.text.empty()) {
                if (g_replaced.size() >= 64) g_replaced.erase(g_replaced.begin());
                g_replaced.emplace_back(d.text, now);
            }
            d.text.assign(text);
            Log("text %p pos %.3f,%.3f: %s", layer, m[12], m[13], ForLog(text).c_str());
        }
        highlighted = color == g_menuColor(true) && (firstSeen || d.color != color);
        d.x = m[12];
        d.y = m[13];
        d.color = color;
        d.lastDrawn = now;
    }
    // A layer coming back into view counts too: returning to a menu should re-read its description.
    if ((changed || firstSeen) && g_onTextChanged) g_onTextChanged(layer, text, m[12], m[13], firstSeen, moved);
    if (highlighted && g_onHighlight) g_onHighlight(layer, m[12], m[13]);
}

std::vector<RowText> TextOnRow(float y) {
    struct Hit {
        RowText row;
        float x;
    };
    std::vector<Hit> hits;
    {
        std::lock_guard lock(g_mutex);
        ULONGLONG now = GetTickCount64();
        for (const auto& [layer, d] : g_drawn) {
            if (!d.text.empty() && now - d.lastDrawn <= kOnScreenMs && std::abs(d.y - y) <= kRowTolerance)
                hits.push_back({{layer, d.text}, d.x});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.x < b.x; });
    std::vector<RowText> out;
    for (Hit& h : hits) out.push_back(std::move(h.row));
    return out;
}

// Other drawables (sprites, card pictures, frames) carry no text, but whether they were drawn tells a shown widget
// from a hidden one. Each class's draw (vtable slot 1) gets its own detour so it can call its own original.
static constexpr const char* kDrawableClasses[] = {".?AVTLayerAnimoo@DFX@@", ".?AVTLayerCustom@DFX@@", ".?AVTLayerFrame@DFX@@",
                                                   ".?AVTLayerFrame2@DFX@@", ".?AVTLayerTexture@DFX@@"};
static DrawFn g_drawableOriginal[std::size(kDrawableClasses)];

static void MarkDrawn(const void* drawable) {
    static unsigned calls;
    std::lock_guard lock(g_mutex);
    g_drawn[drawable].lastDrawn = GetTickCount64();
    if (++calls % 50000 == 0) {  // sprites are many; forget ones gone for a while
        ULONGLONG now = GetTickCount64();
        std::erase_if(g_drawn, [&](const auto& e) { return now - e.second.lastDrawn > 5000; });
    }
}

template <size_t N>
static void __fastcall DrawableHook(void* drawable) {
    g_drawableOriginal[N](drawable);
    MarkDrawn(drawable);
}

template <size_t... N>
static void HookDrawables(std::index_sequence<N...>) {
    std::vector<void*> hooked;
    auto hookOne = [&](size_t n, void* detour) {
        void** vtable = FindVTable(kDrawableClasses[n]);
        if (!vtable || std::find(hooked.begin(), hooked.end(), vtable[1]) != hooked.end()) return;
        if (MH_CreateHook(vtable[1], detour, (void**)&g_drawableOriginal[n]) == MH_OK && MH_EnableHook(vtable[1]) == MH_OK) {
            hooked.push_back(vtable[1]);
        } else {
            Log("Could not hook draw of %s", kDrawableClasses[n]);
        }
    };
    (hookOne(N, (void*)&DrawableHook<N>), ...);
}

bool TextHooksInstall() {
    g_textLayerVTable = FindVTable(".?AVTLayerText@DFX@@");
    void* draw = g_textLayerVTable ? g_textLayerVTable[1] : nullptr;  // slot 1: draw
    g_getMatrix = (GetMatrixFn)FindInGame("GetWorldMatrix",
        "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 C1 E0 06 4C 8D 05 ?? ?? ?? ?? 48 83 C2 C0 48 8B D9 48 03 D0 E8");
    g_menuColor = (MenuColorFn)FindInGame("MenuColor", "8B 05 ?? ?? ?? ?? 84 C9 0F 45 05 ?? ?? ?? ?? C3");
    if (!draw || !g_getMatrix || !g_menuColor) return false;
    Log("TLayerText::Draw at YuGiOh.exe+0x%llX", (unsigned long long)((uint8_t*)draw - (uint8_t*)GetModuleHandleW(nullptr)));

    if (MH_CreateHook(draw, (void*)&DrawHook, (void**)&g_draw) != MH_OK || MH_EnableHook(draw) != MH_OK) {
        Log("Could not hook TLayerText::Draw");
        return false;
    }
    HookDrawables(std::make_index_sequence<std::size(kDrawableClasses)>());
    return true;
}

// Copies of the texts drawn in the last moment.
static std::vector<Drawn> TextsOnScreen() {
    std::vector<Drawn> onScreen;
    std::lock_guard lock(g_mutex);
    ULONGLONG now = GetTickCount64();
    for (auto it = g_drawn.begin(); it != g_drawn.end();) {
        if (now - it->second.lastDrawn > 5000) {
            it = g_drawn.erase(it);  // long gone; drop so the map doesn't grow forever
        } else {
            if (!it->second.text.empty() && now - it->second.lastDrawn <= kOnScreenMs) onScreen.push_back(it->second);
            ++it;
        }
    }
    return onScreen;
}

static void SpeakLogged(const char* what, const std::wstring& text) {
    Log("%s: %s", what, ForLog(text).c_str());
    Speak(ToUtf8(text).c_str());
}

void SpeakStatus() {
    // The player's DP: the amount drawn with the DP icon, and the name drawn just above it (main menu, shop).
    // Prices (shop packs) carry the DP icon too: the player's DP is the one with the name above it, else the
    // one highest on screen.
    std::vector<Drawn> texts = TextsOnScreen();
    const Drawn *best = nullptr, *bestName = nullptr;
    for (const Drawn& dp : texts) {
        if (dp.text.find(kDuelPointsIcon) == std::wstring::npos) continue;
        const Drawn* name = nullptr;
        for (const Drawn& t : texts) {
            if (std::abs(t.x - dp.x) < 0.05f && t.y > dp.y && t.y - dp.y < 0.12f) name = &t;
        }
        if (!best || (name && !bestName) || (!name == !bestName && dp.y > best->y)) {
            best = &dp;
            bestName = name;
        }
    }
    std::wstring out;
    if (best) out = (bestName ? CleanForSpeech(bestName->text) + L", " : L"") + CleanForSpeech(best->text);
    SpeakLogged("SpeakStatus", out.empty() ? L"No DP shown on this screen" : out);
}

void SpeakLifePoints() {
    // Each duelist: an "LP" label, the amount on its row, the name just outside the row. The player's is on the
    // bottom half of the screen.
    std::vector<Drawn> texts = TextsOnScreen();
    std::wstring you, opponent;
    for (const Drawn& label : texts) {
        if (CleanForSpeech(label.text) != L"LP") continue;
        std::wstring amount, name;
        float nameDistance = 1.0f;
        for (const Drawn& t : texts) {
            std::wstring clean = CleanForSpeech(t.text);
            if (clean.empty() || clean == L"LP") continue;
            if (std::abs(t.y - label.y) < kRowTolerance) {
                if (clean.find_first_not_of(L"0123456789") == std::wstring::npos) amount = clean;
            } else if ((t.y > 0) == (label.y > 0) && std::abs(t.y - label.y) < 0.15f && std::abs(t.y - label.y) < nameDistance) {
                name = clean;
                nameDistance = std::abs(t.y - label.y);
            }
        }
        if (amount.empty()) continue;
        if (label.y < 0) {
            you = L"Your LP " + amount;
        } else {
            opponent = (name.empty() ? L"Opponent" : name) + L" LP " + amount;
        }
    }
    std::wstring out = you;
    if (!opponent.empty()) out += (out.empty() ? L"" : L". ") + opponent;
    SpeakLogged("SpeakLifePoints", out.empty() ? L"No life points shown" : out);
}

void SpeakScreen() {
    std::vector<Drawn> onScreen = TextsOnScreen();
    // Positions are clip space: -1..1, y up. Rows top to bottom, then left to right.
    std::sort(onScreen.begin(), onScreen.end(), [](const Drawn& a, const Drawn& b) { return a.y > b.y; });
    for (auto row = onScreen.begin(); row != onScreen.end();) {
        auto rowEnd = std::find_if(row, onScreen.end(), [&](const Drawn& d) { return row->y - d.y > kRowTolerance; });
        std::sort(row, rowEnd, [](const Drawn& a, const Drawn& b) { return a.x < b.x; });
        row = rowEnd;
    }

    std::wstring all;
    for (const Drawn& d : onScreen) {
        std::wstring clean = CleanForSpeech(d.text);
        if (!clean.empty()) all += clean + L"\n";
    }
    if (all.empty()) all = L"No text";
    Log("SpeakScreen (%zu items): %s", onScreen.size(), ForLog(all).c_str());
    Speak(ToUtf8(all).c_str());
}
