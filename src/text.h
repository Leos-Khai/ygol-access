#pragma once
#include <string>
#include <string_view>
#include <vector>

// Hooks the draw function of DFX::TLayerText, the game's UI text element, to record every string
// drawn and where. New and changed strings are logged.
bool TextHooksInstall();

// Speaks all text drawn in the last moment, top to bottom, left to right.
void SpeakScreen();

// Speaks the player's name and DP where the screen shows them (main menu, shop).
void SpeakStatus();

// Speaks both duelists' life points during a duel.
void SpeakLifePoints();

// The vtable of DFX::TLayerText, for recognising text layers. Valid after TextHooksInstall.
extern void** g_textLayerVTable;

// The current string of a DFX::TLayerText. Empty if the layer's string looks invalid.
std::wstring_view LayerText(const void* layer);

// Works for any drawable (text, sprite, card picture): all their draws are hooked.
struct LayerSeen {
    bool known;              // drawn at least once
    unsigned long long age;  // ms since last drawn
    float x, y;              // clip space, y up
};
LayerSeen LayerLastDrawn(const void* layer);

// Called on the game thread when a layer is drawn with different text than last time.
// firstSeen: the layer had never been drawn before. moved: another label showed this text a moment ago (a list
// scrolled and its labels were reused). Position is clip space, y up.
using TextChangedFn = void (*)(const void* layer, std::wstring_view text, float x, float y, bool firstSeen, bool moved);
extern TextChangedFn g_onTextChanged;

// Called on the game thread when a layer starts being drawn in the menu focus colour. Screens that highlight
// their selected row without widget focus (Settings, for one) are caught this way.
using HighlightFn = void (*)(const void* layer, float x, float y);
extern HighlightFn g_onHighlight;

// Layers drawn in the last moment on the same row as y (clip space), left to right.
struct RowText {
    const void* layer;
    std::wstring text;
};
std::vector<RowText> TextOnRow(float y);

// Drops the game's private-use icon characters (button and currency glyphs) and trims spaces.
std::wstring CleanForSpeech(std::wstring_view text);
std::string ToUtf8(std::wstring_view text);
