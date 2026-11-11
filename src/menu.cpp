// Menu focus speech.
//
// Every UI widget derives from RIX::widget_Base, whose vtable slot 1 is SetFocused(widget, bool): it stores the
// flag at widget+8. Classes with their own highlight override the slot but call the base function, so one hook
// on the base sees focus changes for all widget kinds. On a change to focused, the widget's text is read from
// its scene node subtree. Descriptions come from the help bar's setter. An announcer thread orders the two:
// item first (interrupting), then the description queued behind it.
#include "menu.h"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cards.h"
#include "duel.h"
#include "rtti.h"
#include "scan.h"
#include "speech.h"
#include "text.h"

// RIX::widget_Base: +0x08 focused flag, +0x10 element (DFX::TBase*, raw pointer of a shared_ptr).
// DFX::TBase: +0x28 children (MSVC std::set<shared_ptr<TBase>>, head node pointer), +0xB8 content (e.g. a TLayerText).
// widget_Help (prompt bar): +0x58 description TLayerText*.
constexpr size_t kFocusedOffset = 0x08, kElementOffset = 0x10;
constexpr size_t kChildrenOffset = 0x28, kContentOffset = 0xB8;
constexpr size_t kElementFlagsOffset = 0x88;
constexpr uint32_t kElementShown = 0x8;
constexpr size_t kHelpDescriptionOffset = 0x58;
// RIX::widget_Card: +0x100 card id (int16, -1 = none).
constexpr size_t kCardIdOffset = 0x100;

// "Is this card new to the collection?": SetCard (0x140882D80) asks the object in a global (0x14332BD20) through
// its vtable slot 1, (object, card id) -> bool, to draw the NEW mark. The pack screen draws its own mark, so the
// card widget's badge can't be read; the mod asks the same question.
static void* const* g_collection;
static bool IsNewCard(int16_t id) {
    void* object = g_collection ? *g_collection : nullptr;
    if (!object) return false;
    auto isNew = (bool(__fastcall*)(void*, uint16_t))(*(void* const* const*)object)[1];
    return isNew(object, (uint16_t)id);
}

// RIX::widget_CardDisplay (card grid: pack contents, the trunk): +0xE8/+0xF0 its widget_Card array (0x110 bytes
// each), +0x210/+0x218 its card list (12 bytes per card), +0x1B0 the grid cursor: +0x08 columns, +0x10/+0x14
// scroll column/row, +0x18 visible columns (slot -> index as in 0x140869830). Grids are remembered when they take
// focus, and checked by class before each use. Game thread only.
constexpr char kCardGridClass[] = ".?AVwidget_CardDisplay@RIX@@";
static std::vector<const uint8_t*> g_cardGrids;

// "3 of 8" for a card widget inside a remembered grid, or empty.
static std::wstring CardPosition(const uint8_t* card) {
    for (const uint8_t* grid : g_cardGrids) {
        const char* cls = ClassName(grid);
        if (!cls || strcmp(cls, kCardGridClass) != 0) continue;
        auto* begin = *(const uint8_t* const*)(grid + 0xE8);
        auto* end = *(const uint8_t* const*)(grid + 0xF0);
        if (card < begin || card >= end || (card - begin) % 0x110 != 0) continue;
        int slot = (int)((card - begin) / 0x110);
        const uint8_t* cursor = grid + 0x1B0;
        int columns = *(const int*)(cursor + 0x08), visible = *(const int*)(cursor + 0x18);
        if (visible <= 0) return L"";
        int index = columns * (*(const int*)(cursor + 0x14) + slot / visible) + *(const int*)(cursor + 0x10) + slot % visible;
        long long count = (*(const uint8_t* const*)(grid + 0x218) - *(const uint8_t* const*)(grid + 0x210)) / 12;
        if (index < 0 || index >= count) return L"";
        return std::to_wstring(index + 1) + L" of " + std::to_wstring(count);
    }
    return L"";
}

// Focus changes this close together are one announcement (one frame is ~16 ms).
constexpr ULONGLONG kCoalesceMs = 30;
// A description set this long before the focus change still belongs to it.
constexpr ULONGLONG kTipBeforeMs = 50;

using SetFocusedFn = void(__fastcall*)(uint8_t* widget, char focused);
using SetDescriptionFn = void(__fastcall*)(uint8_t* help, intptr_t textIdOrPointer);
static SetFocusedFn g_setFocused;
static SetFocusedFn g_setEnabled;  // same signature: (widget, bool)
// The game's card name lookup (card id -> name in the game's language).
using CardNameFn = const wchar_t*(__fastcall*)(int16_t cardId);
static CardNameFn g_cardName;
static CardNameFn g_cardText;  // same signature: card id -> its text

std::wstring GameCardName(int16_t id) {
    const wchar_t* name = g_cardName && id > 0 ? g_cardName(id) : nullptr;
    return name ? CleanForSpeech(name) : L"";
}
std::wstring GameCardText(int16_t id) {
    const wchar_t* text = g_cardText && id > 0 ? g_cardText(id) : nullptr;
    return text ? CleanForSpeech(text) : L"";
}
static SetDescriptionFn g_setDescription;

// ---- Reading a widget's text -------------------------------------------------------------------

struct SetNode {  // MSVC std::_Tree_node
    SetNode* left;
    SetNode* parent;
    SetNode* right;
    char color;
    char isNil;
    char pad[6];
    uint8_t* value;  // the child TBase* (shared_ptr raw pointer)
};

// Collects every drawable in a node's subtree, and separately the text layers among them.
static void CollectLayers(const uint8_t* node, std::vector<const void*>& drawables, std::vector<const void*>& layers, int& budget) {
    if (!node || --budget < 0) return;  // budget guards against a corrupt or cyclic tree
    auto* content = *(void* const*)(node + kContentOffset);
    if (content) {
        drawables.push_back(content);
        if (*(void* const*)content == g_textLayerVTable) layers.push_back(content);
    }

    auto* head = *(SetNode* const*)(node + kChildrenOffset);
    if (!head) return;
    for (SetNode* it = head->left; it && !it->isNil && budget > 0;) {  // in-order walk, head->left is the first node
        CollectLayers(it->value, drawables, layers, budget);
        if (!it->right->isNil) {
            it = it->right;
            while (!it->left->isNil) it = it->left;
        } else {
            SetNode* up = it->parent;
            while (!up->isNil && it == up->right) {
                it = up;
                up = up->parent;
            }
            it = up;
        }
    }
}

// Piece layer for text without a layer that belongs after the widget's own labels (a filter option's "Selected").
static const void* const kAfterText = (const void*)1;

// A focused widget's text, captured on the game thread. Layer pointers are only used afterwards as keys for
// LayerLastDrawn, never dereferenced, so a widget freed in the meantime is harmless.
struct Focus {
    struct Piece {
        const void* layer;  // nullptr: text not on screen as such (a card's name, a volume level)
        std::wstring text;
    };
    std::vector<Piece> pieces;
    std::vector<const void*> drawables;  // everything in the widget; empty = nothing to check
    ULONGLONG at;
    const char* cls = nullptr;  // decorated RTTI name, if from a widget
    bool startedEmpty = false;  // no text when it took focus: its labels are being filled in (a shop pack)
    bool card = false;          // a card, spoken with its name, summary and text
    bool highlight = false;     // from a label drawn in the focus colour (OnHighlight), at row y
    bool relabel = false;       // the focused row's labels changing as its list scrolls
    float y = 0;
};

// Comparable form: letters and digits only, lower case. The same name can come with different dashes or spaces.
static std::wstring Normalize(std::wstring_view text) {
    std::wstring out;
    for (wchar_t c : text) {
        if (iswalnum(c)) out += towlower(c);
    }
    return out;
}

static bool Contains(std::wstring_view haystack, std::wstring_view needle) {
    std::wstring n = Normalize(needle);
    return !n.empty() && Normalize(haystack).find(n) != std::wstring::npos;
}

static Focus WidgetFocus(const uint8_t* widget) {
    std::vector<const void*> layers;
    Focus focus{{}, {}, GetTickCount64()};
    int budget = 512;
    CollectLayers(*(const uint8_t* const*)(widget + kElementOffset), focus.drawables, layers, budget);
    for (const void* layer : layers) {
        std::wstring text = CleanForSpeech(LayerText(layer));
        if (!text.empty()) focus.pieces.push_back({layer, std::move(text)});
    }
    return focus;
}

static bool DrawnSince(const void* drawable, ULONGLONG sinceMs) {
    LayerSeen seen = LayerLastDrawn(drawable);
    return seen.known && seen.age <= sinceMs;
}

// The focus's text if it is on screen: something in it was drawn since the focus change. Drawing is what tells
// shown from hidden: screens are built off-screen at startup and set focus there, and widgets keep labels they
// hide. Text pieces not drawn are left out; pieces without a layer come first. Then top to bottom, left to right.
// Pieces already said in `spoken` (a details panel repeating the card name) are left out too.
static std::wstring VisibleText(const Focus& f, std::wstring_view spoken = {}) {
    ULONGLONG sinceFocus = GetTickCount64() - f.at;
    bool onScreen = f.drawables.empty() ||
        std::any_of(f.drawables.begin(), f.drawables.end(), [&](const void* d) { return DrawnSince(d, sinceFocus); });
    if (!onScreen) return L"";
    struct Shown {
        std::wstring text;
        LayerSeen seen;
    };
    std::vector<Shown> shown;
    for (const Focus::Piece& p : f.pieces) {
        if (Contains(spoken, p.text)) continue;
        // No layer: sorts first (a card's name); kAfterText: sorts last (an option's "Selected").
        LayerSeen seen = p.layer == kAfterText ? LayerSeen{true, 0, 2.0f, -2.0f}
                       : p.layer             ? LayerLastDrawn(p.layer)
                                             : LayerSeen{true, 0, -2.0f, 2.0f};
        if (!seen.known || seen.age > sinceFocus) continue;
        std::wstring text = p.text;
        // A details block often opens with the name just spoken ("Name\n[Type]\nText"): drop that opening line.
        if (size_t nl = text.find(L'\n'); nl != std::wstring::npos && Contains(spoken, std::wstring_view(text).substr(0, nl))) {
            text = CleanForSpeech(std::wstring_view(text).substr(nl + 1));
            if (text.empty()) continue;
        }
        shown.push_back({std::move(text), seen});
    }
    std::stable_sort(shown.begin(), shown.end(), [](const Shown& a, const Shown& b) {
        if (std::abs(a.seen.y - b.seen.y) > 0.03f) return a.seen.y > b.seen.y;
        return a.seen.x < b.seen.x;
    });
    std::wstring out;
    std::vector<std::wstring> said;
    for (const Shown& s : shown) {
        // The same label drawn twice (a shadow). Exact repeats only: a count "3" after "Level 3" is real.
        std::wstring key = Normalize(s.text);
        if (std::find(said.begin(), said.end(), key) != said.end()) continue;
        said.push_back(std::move(key));
        if (!out.empty()) out += wcschr(L".!?", out.back()) ? L" " : L", ";  // no comma after a full stop
        out += s.text;
    }
    return out;
}

// Booster packs show two fractions, told apart only by icons: different cards collected of the pack's cards,
// then copies owned of three of each.
static std::wstring LabelPackCounts(const std::wstring& text) {
    static const std::wregex fraction(L"(\\d+)/(\\d+)");
    std::wstring out;
    int n = 0;
    auto last = text.cbegin();
    for (std::wsregex_iterator it(text.begin(), text.end(), fraction), end; it != end; ++it, ++n) {
        out.append(last, (*it)[0].first);
        out += (*it)[1].str() + L" of " + (*it)[2].str() + (n == 0 ? L" cards" : L" copies");
        last = (*it)[0].second;
    }
    out.append(last, text.cend());
    return out;
}

static std::wstring AllText(const Focus& f) {
    std::wstring out;
    for (const Focus::Piece& p : f.pieces) out += (out.empty() ? L"" : L" | ") + p.text;
    return out;
}

// ---- Announcer ---------------------------------------------------------------------------------

// How long a focus may wait for its text to be drawn (a new screen's first frames).
constexpr ULONGLONG kDrawWaitMs = 300;
// How long a widget that took focus with no text gets to fill in all its labels.
constexpr ULONGLONG kLabelFillMs = 150;

static std::mutex g_mutex;
static std::condition_variable g_wake;
static std::vector<Focus> g_items;
static Focus g_tipFocus;  // the description pieces, with their layers for the on-screen check
static ULONGLONG g_tipAt, g_lastItemSpokenAt, g_lastFocusAt;
static std::wstring g_lastItemSpoken;
static Focus g_live;  // text changed with no focus change (story lines, messages), being collected
static ULONGLONG g_lastScrollAt;  // when a list last reused its labels
static std::vector<const void*> g_liveLayers;  // layers that have carried live prose (story boxes, message lines)
static std::wstring g_elsewhere;               // the duel code's last announcement
static ULONGLONG g_elsewhereAt;
static std::vector<std::pair<std::wstring, ULONGLONG>> g_spokenItems;  // recent items, to catch flicker
static std::atomic<ULONGLONG> g_anyKeyAt;  // the last key press while the game had focus
static std::atomic<ULONGLONG> g_gameKeyAt;  // the last one the game saw (not one of the mod's hotkeys)
static std::wstring g_suppressLive;        // text starting with this is dropped (MenuSuppressLive)
static ULONGLONG g_suppressLiveAt;
static std::atomic<ULONGLONG> g_messageAt;  // the duel just opened a message box (MenuExpectMessage)
static bool g_tipPending;
static std::vector<const void*> g_focusLayers;        // layers of the latest focus: never its description
static std::vector<const void*> g_descriptionLayers;  // layers the help bar's setter wrote to
static std::vector<std::pair<float, float>> g_descriptionSlots;  // where descriptions have appeared
static std::vector<std::wstring> g_recentItems;                  // texts of the last few focused items

static bool g_lastItemWasCard;  // the last item spoken was a card
static std::wstring g_cardInfo;  // the focused card's full info for the I key; cleared when focus leaves cards
static std::vector<const void*> g_sortLayers;  // labels of sort bars: a change is announced as "Sort by: ..."

static void SpeakW(const std::wstring& text, bool interrupt);

void MenuSpeakCardInfo() {
    std::wstring info;
    {
        std::lock_guard lock(g_mutex);
        info = g_cardInfo;
    }
    SpeakW(info.empty() ? L"No card selected" : info, true);
}

// After a card, the details panel only repeats it: keep just the help bar's description.
static void KeepKnownDescriptionsLocked(Focus& f) {
    std::erase_if(f.pieces, [](const Focus::Piece& p) {
        return std::find(g_descriptionLayers.begin(), g_descriptionLayers.end(), p.layer) == g_descriptionLayers.end();
    });
}

// A list scrolled at about the same moment: the row coming into view is new text, but neither a description
// nor news. Keeps only the help bar's description and the focused item's labels.
static void DropScrolledLocked(Focus& f, ULONGLONG at) {
    if ((at > g_lastScrollAt ? at - g_lastScrollAt : g_lastScrollAt - at) > 100) return;
    std::erase_if(f.pieces, [](const Focus::Piece& p) {
        return std::find(g_descriptionLayers.begin(), g_descriptionLayers.end(), p.layer) == g_descriptionLayers.end() &&
               std::find(g_focusLayers.begin(), g_focusLayers.end(), p.layer) == g_focusLayers.end();
    });
}

// Text changes this soon after a focus change can be its description.
constexpr ULONGLONG kTipWindowMs = 500;

// Removes focuses that contain another focus's drawables (a container of a focused entry).
static void DropContainers(std::vector<Focus>& items) {
    auto contains = [](const Focus& outer, const Focus& inner) {
        if (&outer == &inner || inner.drawables.empty() || outer.drawables.size() <= inner.drawables.size()) return false;
        return std::all_of(inner.drawables.begin(), inner.drawables.end(), [&](const void* d) {
            return std::find(outer.drawables.begin(), outer.drawables.end(), d) != outer.drawables.end();
        });
    };
    // Toolbars (sort and filter bars) gain focus alongside the section's first entry; the entry is what matters.
    auto isBar = [](const Focus& f) {
        if (!f.cls) return false;
        std::string_view name(f.cls);
        name = name.substr(4, name.find('@') - 4);  // ".?AVwidget_X@..." -> "widget_X"
        return name.ends_with("Bar") || name == "widget_ButtonHelp";  // and helper buttons
    };
    bool hasNonBar = std::any_of(items.begin(), items.end(), [&](const Focus& f) { return !isBar(f); });
    std::vector<Focus> kept;
    for (const Focus& f : items) {
        if (hasNonBar && isBar(f)) continue;
        if (std::none_of(items.begin(), items.end(), [&](const Focus& other) { return contains(f, other); })) kept.push_back(f);
    }
    items = std::move(kept);
}

static void SpeakW(const std::wstring& plain, bool interrupt) {
    std::wstring text = DuelAnnotate(plain);
    Log("speak%s: %s", interrupt ? "" : " (queued)", ToUtf8(text).c_str());
    Speak(ToUtf8(text).c_str(), interrupt);
}

static void AnnouncerThread() {
    std::unique_lock lock(g_mutex);
    for (;;) {
        g_wake.wait(lock, [] { return !g_items.empty() || g_tipPending || !g_live.pieces.empty(); });
        ULONGLONG now = GetTickCount64();
        auto waitMs = [&](ULONGLONG ms) { g_wake.wait_for(lock, std::chrono::milliseconds(ms)); };

        if (!g_live.pieces.empty() && g_items.empty() && !g_tipPending) {
            if (now < g_live.at + kCoalesceMs) {
                waitMs(g_live.at + kCoalesceMs - now);
                continue;
            }
            Focus live = std::move(g_live);
            g_live = {};
            DropScrolledLocked(live, live.at);
            // Drawn together with a focus change: a screen appearing, which the focus announcement covers (if it
            // said anything: a focused row still off screen, as on the Duel Result screen, doesn't).
            if ((live.at > g_lastFocusAt ? live.at - g_lastFocusAt : g_lastFocusAt - live.at) < 150 &&
                g_lastItemSpokenAt + 150 >= live.at)
                continue;
            auto inList = [](const std::vector<const void*>& list, const void* layer) {
                return std::find(list.begin(), list.end(), layer) != list.end();
            };
            auto isProse = [](const Focus::Piece& p) { return std::count(p.text.begin(), p.text.end(), L' ') >= 2; };  // 3+ words: "Press to unwrap!"
            if (live.pieces.size() > 3) {
                // Many labels at once: a screen appearing or coming back (a duel, the duel list after a story).
                // Only the item the cursor is on and sentences (a duel's opening message) are worth saying.
                std::erase_if(live.pieces, [&](const Focus::Piece& p) { return !inList(g_focusLayers, p.layer) && !isProse(p); });
            } else if (std::any_of(live.pieces.begin(), live.pieces.end(), isProse)) {
                // Prose: say the batch (e.g. a speaker's name with the line) and remember its layers, so later
                // short lines in the same story box count too.
                for (const Focus::Piece& p : live.pieces) {
                    if (!inList(g_liveLayers, p.layer)) g_liveLayers.push_back(p.layer);
                }
                if (g_liveLayers.size() > 64) g_liveLayers.erase(g_liveLayers.begin(), g_liveLayers.end() - 64);
            } else {
                // No prose: only layers that carried prose before (a short story line). A counter or a label
                // changing on its own is not worth interrupting for.
                std::erase_if(live.pieces, [&](const Focus::Piece& p) { return !inList(g_liveLayers, p.layer); });
            }
            bool speak = !live.pieces.empty();
            std::wstring text = speak ? VisibleText(live) : L"";
            if (text.empty()) continue;
            if (now - g_elsewhereAt < 2000 && Contains(g_elsewhere, text)) continue;  // a banner repeating a duel event
            lock.unlock();
            SpeakW(text, true);
            lock.lock();
            continue;
        }

        if (!g_items.empty()) {
            ULONGLONG first = g_items.front().at;
            if (now < first + kCoalesceMs) {  // let same-frame focus changes and a late description arrive
                waitMs(first + kCoalesceMs - now);
                continue;
            }
            // A list and the entry in it can gain focus together; the entry is what the user is on.
            DropContainers(g_items);
            // Switching section (deck editor) focuses the new section's card, then restores the cursor card of the
            // section left behind. The first is where the user is.
            if (auto firstCard = std::find_if(g_items.begin(), g_items.end(), [](const Focus& f) { return f.card; }); firstCard != g_items.end()) {
                g_items.erase(std::remove_if(firstCard + 1, g_items.end(), [](const Focus& f) { return f.card; }), g_items.end());
                // A card details panel focused with it (opening a pack) only repeats the card.
                std::erase_if(g_items, [](const Focus& f) { return f.cls && strstr(f.cls, "CardDetails"); });
            }
            // Highlighted labels on two or more rows at once are a colour scheme (the duel's LP readouts), not a
            // cursor. Drop them, and forget they counted as a focus change so text right after is still news.
            std::vector<float> rows;
            for (const Focus& f : g_items) {
                if (f.highlight && std::none_of(rows.begin(), rows.end(), [&](float y) { return std::abs(y - f.y) < 0.03f; }))
                    rows.push_back(f.y);
            }
            // Highlights are the fallback for screens without widget focus; with a widget focus they are extra labels.
            bool widgetFocus = std::any_of(g_items.begin(), g_items.end(), [](const Focus& f) { return f.cls != nullptr; });
            if (rows.size() >= 2 || (widgetFocus && !rows.empty())) {
                std::erase_if(g_items, [](const Focus& f) { return f.highlight; });
                if (g_items.empty()) {
                    g_lastFocusAt = 0;
                    g_focusLayers.clear();
                    continue;
                }
            }
            // Keep focuses whose text got drawn; wait a little for ones that may still be on their way in. A widget
            // with no text yet may get its labels in the next frames; wait for it only if nothing else has text.
            bool undecided = false, waitingEmpty = false;
            // A focus that had text from the start (a main menu item) is ready; empty helpers alongside it don't wait.
            bool othersHaveText = std::any_of(g_items.begin(), g_items.end(), [](const Focus& f) { return !f.startedEmpty && !f.pieces.empty(); });
            std::wstring item;
            for (const Focus& f : g_items) {
                std::wstring text = VisibleText(f);
                if (f.cls && strstr(f.cls, "widget_PackWrapper")) text = LabelPackCounts(text);
                if (f.startedEmpty && now < f.at + kLabelFillMs && !text.empty() && !othersHaveText) {
                    undecided = true;  // the only text is labels still being filled in: give the rest time to arrive
                } else if (!text.empty()) {
                    if (Contains(item, text)) {
                    } else if (text.starts_with(L"Sort by ")) {
                        item = text + (item.empty() ? L"" : L", ") + item;  // the cause first, then the card it brought
                    } else {
                        item += (item.empty() ? L"" : L", ") + text;
                    }
                } else if (now < f.at + kDrawWaitMs) {
                    (f.pieces.empty() ? waitingEmpty : undecided) = true;
                } else if (f.cls && strstr(f.cls, "widget_CampaignDuel") && item.empty() &&
                           std::any_of(f.drawables.begin(), f.drawables.end(), [&](const void* d) { return DrawnSince(d, now - f.at); })) {
                    item = L"Not available";  // the campaign's empty slot past the last duel: drawn, but no title
                }
            }
            if (waitingEmpty && item.empty()) undecided = true;
            if (undecided) {
                waitMs(20);
                continue;
            }
            g_lastItemWasCard = std::any_of(g_items.begin(), g_items.end(), [](const Focus& f) { return f.card; });
            // Only the focuses kept count as "the focused item" (dropped containers' badges are not).
            g_focusLayers.clear();
            for (const Focus& f : g_items) {
                for (const Focus::Piece& p : f.pieces) {
                    if (p.layer) g_focusLayers.push_back(p.layer);
                }
            }
            // A duel's Yes/No buttons are highlights; the card the prompt is about is the one the duel last showed.
            bool duelButton = std::all_of(g_items.begin(), g_items.end(), [](const Focus& f) { return f.highlight; }) && DuelInProgress();
            float buttonRow = g_items.front().y;
            bool reward = std::any_of(g_items.begin(), g_items.end(), [](const Focus& f) { return f.cls && strstr(f.cls, "widget_RewardWindow"); });
            if (!g_lastItemWasCard && !duelButton) g_cardInfo.clear();  // I must not read a card the user has left
            std::wstring tip;
            if (g_tipPending && g_tipAt + kTipBeforeMs >= first) {  // the description belongs to this focus change
                DropScrolledLocked(g_tipFocus, g_tipAt);
                if (g_lastItemWasCard) KeepKnownDescriptionsLocked(g_tipFocus);
                if (duelButton) {  // the other buttons on the row aren't part of the question
                    std::erase_if(g_tipFocus.pieces, [&](const Focus::Piece& p) {
                        LayerSeen seen = p.layer ? LayerLastDrawn(p.layer) : LayerSeen{};
                        return seen.known && std::abs(seen.y - buttonRow) < 0.03f;
                    });
                }
                tip = VisibleText(g_tipFocus, item);
                g_tipPending = false;
            }
            // A box the duel engine didn't send (S: "Would you like to exit this tutorial?") has no message event to
            // make its question the description: it is drawn with the buttons, as live text.
            if (duelButton && tip.empty()) {
                Focus question{{}, {}, g_live.at};
                std::erase_if(g_live.pieces, [&](Focus::Piece& p) {
                    bool asks = !p.text.empty() && p.text.back() == L'?';
                    if (asks) question.pieces.push_back(p);
                    return asks;
                });
                tip = VisibleText(question, item);
            }
            g_items.clear();
            // A reward's label ("Card Award", "DP") only repeats the line that says it ("NEW CARD AWARD! Evo-Force").
            if (reward && !tip.empty()) {
                item = std::move(tip);
                tip.clear();
            }
            // "Activate the effect of X?": the question first, then the button the cursor is on.
            bool questionFirst = duelButton && !tip.empty() && tip.back() == L'?';
            if (item.empty() && !tip.empty()) {  // a picture took focus (a duel's phase icon): its dialog's text says it
                item = std::move(tip);
                tip.clear();
            }
            if (item.empty()) continue;  // all off-screen
            // Focus flickering between items with no key pressed (the opponent choosing first or second): an
            // item said within the last second is not said again.
            std::erase_if(g_spokenItems, [&](const auto& s) { return now - s.second > 1000; });
            bool flicker = now - g_anyKeyAt > 500 &&
                std::any_of(g_spokenItems.begin(), g_spokenItems.end(), [&](const auto& s) { return s.first == item; });
            g_spokenItems.emplace_back(item, now);
            if (flicker) continue;
            g_lastItemSpokenAt = now;
            g_lastItemSpoken = item;
            lock.unlock();
            if (questionFirst) std::swap(item, tip);
            SpeakW(item, true);
            if (!tip.empty()) SpeakW(tip, false);
            lock.lock();
        } else {
            // A description after its item was spoken, or before its focus change: wait briefly for more pieces
            // and for the item.
            ULONGLONG hold = now - g_lastItemSpokenAt > kTipBeforeMs ? kTipBeforeMs : kCoalesceMs;
            if (now < g_tipAt + hold) {
                waitMs(g_tipAt + hold - now);
                continue;
            }
            DropScrolledLocked(g_tipFocus, g_tipAt);
            if (g_lastItemWasCard && g_tipAt - g_lastItemSpokenAt < kTipWindowMs) KeepKnownDescriptionsLocked(g_tipFocus);
            std::wstring tip = VisibleText(g_tipFocus, g_lastItemSpoken);
            if (tip.empty() && now < g_tipAt + kDrawWaitMs) {
                waitMs(20);
                continue;
            }
            g_tipPending = false;
            if (tip.empty()) continue;
            lock.unlock();
            SpeakW(tip, false);  // queued: never cut off the item it follows
            lock.lock();
        }
    }
}

static void AnnounceItem(Focus focus) {
    std::lock_guard lock(g_mutex);
    ULONGLONG now = GetTickCount64();
    if (now - g_lastFocusAt > kCoalesceMs) g_focusLayers.clear();  // same-frame focuses share the exclusion list
    for (const Focus::Piece& p : focus.pieces) {
        g_focusLayers.push_back(p.layer);
        g_recentItems.push_back(p.text);
    }
    if (g_recentItems.size() > 8) g_recentItems.erase(g_recentItems.begin(), g_recentItems.end() - 8);
    g_lastFocusAt = now;
    g_items.push_back(std::move(focus));
    g_wake.notify_one();
}

// Adds a description piece; one per layer (the setter and the draw hook both report the help bar's).
static void AddTipLocked(const void* layer, std::wstring text) {
    ULONGLONG now = GetTickCount64();
    if (!g_tipPending) {
        g_tipFocus = {{}, {}, now};
        g_tipAt = now;
        g_tipPending = true;
    }
    auto same = std::find_if(g_tipFocus.pieces.begin(), g_tipFocus.pieces.end(), [&](const auto& p) { return p.layer == layer; });
    if (same != g_tipFocus.pieces.end()) {
        same->text = std::move(text);
    } else {
        g_tipFocus.pieces.push_back({layer, std::move(text)});
    }
    g_wake.notify_one();
}

// ---- Hooks (game thread) -----------------------------------------------------------------------

constexpr char kCardClass[] = ".?AVwidget_Card@RIX@@";

// "First. Second", without doubling a full stop the first already ends with.
static void AppendSentence(std::wstring& out, const std::wstring& next) {
    if (next.empty()) return;
    if (!out.empty()) out += (out.back() == L'.' || out.back() == L'!' || out.back() == L'?') ? L" " : L". ";
    out += next;
}

// A card shows its name as part of its picture, so it is looked up by the card id the widget holds. Browsing
// says the name (and New, and where it is); I says the rest: kind/attribute/type/level/ATK/DEF (the details
// panel shows those as bare numbers or pictures), the card's text and its count badge.
static void MakeCardFocus(const uint8_t* widget, Focus& focus) {
    int16_t id = *(const int16_t*)(widget + kCardIdOffset);
    const wchar_t* name = id != -1 && g_cardName ? g_cardName(id) : nullptr;
    if (!name || !*name) return;
    std::wstring info = CleanForSpeech(name);
    std::vector<Focus::Piece> card{{nullptr, info}};
    if (IsNewCard(id)) card.push_back({nullptr, L"New"});
    if (std::wstring position = CardPosition(widget); !position.empty()) card.push_back({nullptr, position});
    AppendSentence(info, CardSummary(id));
    if (const wchar_t* text = g_cardText ? g_cardText(id) : nullptr; text && *text) AppendSentence(info, CleanForSpeech(text));
    for (const Focus::Piece& badge : focus.pieces) AppendSentence(info, badge.text + L" copies");
    focus.pieces = std::move(card);
    focus.card = true;
    std::lock_guard lock(g_mutex);
    g_cardInfo = std::move(info);
}

// The cursor stays on a card widget while the card in it changes (the trunk scrolls past its last visible row,
// or is re-sorted): announce the new card as if focus had moved.
// Only the card that last took focus: moving Right onto the next row scrolls the grid, and the card just left is
// refilled while it still has its focused flag. Game thread only; compared, never read.
static const uint8_t* g_focusedCard;
using SetCardFn = void(__fastcall*)(uint8_t* widget, int16_t cardId);
static SetCardFn g_setCard;
static void __fastcall SetCardHook(uint8_t* widget, int16_t cardId) {
    int16_t before = *(const int16_t*)(widget + kCardIdOffset);
    g_setCard(widget, cardId);
    if (cardId == before || cardId == -1 || !widget[kFocusedOffset] || widget != g_focusedCard) return;
    const char* cls = ClassName(widget);
    if (!cls || strcmp(cls, kCardClass) != 0) return;
    Focus focus = WidgetFocus(widget);
    focus.cls = cls;
    MakeCardFocus(widget, focus);
    Log("card changed under cursor: %s", ToUtf8(AllText(focus)).c_str());
    if (focus.card) AnnounceItem(std::move(focus));
}

// Deck editor stats bar (RIX::widget_DeckStats): Refresh (0x1408973A0) asks its source (+0x2D0, the deck) for 10
// counts through vtable slot 2 and writes them into labels at +0x30 + i*0x20 + 0x10. The order, from the bar's
// colours (the Starter Deck's three Effect monsters are in the orange box): Normal (yellow), Effect (orange),
// Ritual (blue), Fusion (purple), Synchro (white), Spell (green), Trap (pink), Xyz (black), Pendulum, Link.
static const wchar_t* const kStatCategories[10] = {L"Normal", L"Effect", L"Ritual", L"Fusion", L"Synchro",
                                                    L"Spell", L"Trap", L"Xyz", L"Pendulum", L"Link"};
static std::wstring g_deckStats;                  // "3 Effect, 12 Spell", from the last refresh
static std::vector<const void*> g_deckStatsLayers;  // its labels, to tell whether the bar is on screen

using DeckStatsRefreshFn = void(__fastcall*)(uint8_t* stats, void* a, void* b, void* c);
static DeckStatsRefreshFn g_deckStatsRefresh;
static void __fastcall DeckStatsRefreshHook(uint8_t* stats, void* a, void* b, void* c) {
    g_deckStatsRefresh(stats, a, b, c);
    void* source = *(void**)(stats + 0x2D0);
    if (!source) return;
    auto count = (int(__fastcall*)(void*, int))(*(void* const* const*)source)[2];
    std::wstring summary;
    std::vector<const void*> layers;
    for (int i = 0; i < 10; ++i) {
        if (int n = count(source, i); n > 0) summary += (summary.empty() ? L"" : L", ") + std::to_wstring(n) + L" " + kStatCategories[i];
        layers.push_back(*(void**)(stats + 0x30 + i * 0x20 + 0x10));
    }
    std::lock_guard lock(g_mutex);
    g_deckStats = summary.empty() ? L"No cards" : summary;
    g_deckStatsLayers = std::move(layers);
}

bool MenuSpeakDeckStats() {
    std::wstring stats;
    {
        std::lock_guard lock(g_mutex);
        bool shown = std::any_of(g_deckStatsLayers.begin(), g_deckStatsLayers.end(),
                                 [](const void* l) { LayerSeen s = LayerLastDrawn(l); return s.known && s.age < 500; });
        if (!shown) return false;
        stats = g_deckStats;
    }
    SpeakW(L"Deck: " + stats, true);
    return true;
}

// Deck editor filter options (widget_TrunkFilterMaskOption): a selected option shows a check mark, sprite 0x150 in
// its sprite slot at +0xA0 (index at +0x24). RefreshCheck (0x1408BAC10) re-evaluates it after a toggle.
constexpr char kFilterOptionClass[] = ".?AVwidget_TrunkFilterMaskOption@RIX@@";
constexpr size_t kOptionCheckOffset = 0xA0 + 0x24;
constexpr int kCheckedSprite = 0x150;

using RefreshCheckFn = void(__fastcall*)(uint8_t* option);
static RefreshCheckFn g_refreshCheck;
static void __fastcall RefreshCheckHook(uint8_t* option) {
    int before = *(const int*)(option + kOptionCheckOffset);
    g_refreshCheck(option);
    int after = *(const int*)(option + kOptionCheckOffset);
    if (before == after || !option[kFocusedOffset]) return;
    AnnounceItem({{{nullptr, after == kCheckedSprite ? L"Selected" : L"Not selected"}}, {}, GetTickCount64()});
}

static void __fastcall SetFocusedHook(uint8_t* widget, char focused) {
    bool was = widget[kFocusedOffset] != 0;
    g_setFocused(widget, focused);
    // Video Settings unfocuses and refocuses Apply/Back every frame: a widget back within 100 ms never really
    // left. Only the game thread calls this, so the map needs no lock.
    static std::unordered_map<const void*, ULONGLONG> lostAt;
    ULONGLONG now = GetTickCount64();
    if (!focused && was) {
        if (lostAt.size() > 256) lostAt.clear();  // widgets come and go; old entries are useless
        lostAt[widget] = now;
    }
    if (!focused || was) return;  // only a change to focused; some menus call this every frame
    if (auto it = lostAt.find(widget); it != lostAt.end() && now - it->second < 100) return;

    Focus focus = WidgetFocus(widget);
    const char* cls = ClassName(widget);
    focus.cls = cls;
    if (cls && strcmp(cls, kCardGridClass) == 0 && std::find(g_cardGrids.begin(), g_cardGrids.end(), widget) == g_cardGrids.end()) {
        if (g_cardGrids.size() >= 8) g_cardGrids.erase(g_cardGrids.begin());
        g_cardGrids.push_back(widget);
    }
    if (cls && strcmp(cls, kCardClass) == 0) {
        g_focusedCard = widget;
        MakeCardFocus(widget, focus);
    }
    if (cls && strcmp(cls, kFilterOptionClass) == 0 && *(const int*)(widget + kOptionCheckOffset) == kCheckedSprite)
        focus.pieces.push_back({kAfterText, L"Selected"});
    // Sort bars change their label on a key press (X in the trunk); remember the labels to announce that.
    if (cls && strstr(cls, "SortBar")) {
        std::lock_guard lock(g_mutex);
        for (const Focus::Piece& p : focus.pieces) {
            if (std::find(g_sortLayers.begin(), g_sortLayers.end(), p.layer) == g_sortLayers.end()) g_sortLayers.push_back(p.layer);
        }
        if (g_sortLayers.size() > 32) g_sortLayers.erase(g_sortLayers.begin(), g_sortLayers.end() - 32);
    }
    focus.startedEmpty = focus.pieces.empty();
    Log("focus %s %p: %s", cls ? cls : "?", widget, ToUtf8(AllText(focus)).c_str());
    // Even without text yet: some widgets fill in their labels after taking focus (OnTextChanged adds them).
    if (!focus.drawables.empty()) AnnounceItem(std::move(focus));
}

// Popups (message boxes, Yes/No, duel and report windows) often have no focus movement: their buttons are bound to
// keys. widget_Base::SetEnabled shows a widget by setting bit 0x8 in its element's flags; when a popup class goes
// from hidden to shown, its whole text is announced.
static bool IsPopupClass(const char* decorated) {
    if (!decorated || strncmp(decorated, ".?A", 3) != 0) return false;
    std::string_view name(decorated + 4);  // ".?AV" or ".?AU", then the class's own name up to the first '@'
    name = name.substr(0, name.find('@'));
    return name.find("Dialog") != std::string_view::npos || name.find("Window") != std::string_view::npos;
}

static void __fastcall SetEnabledHook(uint8_t* widget, char enabled) {
    auto* element = *(uint8_t* const*)(widget + kElementOffset);
    bool was = element && (*(const uint32_t*)(element + kElementFlagsOffset) & kElementShown);
    g_setEnabled(widget, enabled);
    if (!enabled || was || !element) return;
    const char* cls = ClassName(widget);
    if (!IsPopupClass(cls)) return;
    if (strstr(cls, "SortWindow_t")) DuelSortWindowShown(widget);
    // A card list's hidden Filter and Sort windows are re-shown every frame: only a first appearance is a popup.
    static std::unordered_map<const void*, ULONGLONG> shownAt;  // game thread only
    ULONGLONG now = GetTickCount64();
    ULONGLONG& last = shownAt[widget];
    bool again = now - last < 1000;
    last = now;
    if (again) return;

    Focus popup = WidgetFocus(widget);
    Log("popup %s %p: %s", cls, widget, ToUtf8(AllText(popup)).c_str());
    if (popup.pieces.empty()) return;
    // A duel prompt ("Select position for ...", "Select a card to activate.") is about where the cursor already is.
    if (strstr(cls, "widget_DialogWindow"))
        if (std::wstring cursor = DuelCursorText(AllText(popup)); !cursor.empty()) popup.pieces.push_back({kAfterText, std::move(cursor)});
    AnnounceItem(std::move(popup));
}

static void __fastcall SetDescriptionHook(uint8_t* help, intptr_t textIdOrPointer) {
    auto* layer = *(const void* const*)(help + kHelpDescriptionOffset);
    std::wstring before = layer ? std::wstring(LayerText(layer)) : L"";
    g_setDescription(help, textIdOrPointer);
    if (!layer) return;
    std::wstring after = CleanForSpeech(LayerText(layer));
    if (after.empty() || after == CleanForSpeech(before)) return;  // some screens set it every frame
    Log("description: %s", ToUtf8(after).c_str());
    std::lock_guard lock(g_mutex);
    if (std::find(g_descriptionLayers.begin(), g_descriptionLayers.end(), layer) == g_descriptionLayers.end())
        g_descriptionLayers.push_back(layer);  // its position is learned as a slot when it is drawn
    AddTipLocked(layer, std::move(after));
}

// Descriptions that don't go through the help bar's setter (MenuKit menus store a text id that the layer resolves
// when drawn) are caught where every text change shows: the draw hook. A layer outside the focused widget whose
// text changes just after a focus change is that item's description, and its position becomes a description slot.
// On a screen's first focus all its layers are new, so a new layer only counts if it sits at a known slot.
static bool IsIcon(wchar_t c) { return c >= 0xE000 && c <= 0xF8FF; }

static bool KeysOnly(std::wstring_view text) {
    return std::all_of(text.begin(), text.end(), [](wchar_t c) { return IsIcon(c) || c == L' '; });
}

// The text on row y. Text opening with an icon is a key prompt sharing the row ("[Enter] OK") and is left out,
// except key icons alone (a Controls row's keys) and `label` (null: every label), said without its icon.
static Focus RowFocus(float y, const void* label) {
    Focus row{{}, {}, GetTickCount64()};
    row.y = y;
    for (RowText& r : TextOnRow(y)) {
        std::wstring_view text = r.text;
        if (IsIcon(text.front()) && !KeysOnly(text)) {
            if (label && r.layer != label) continue;
            while (!text.empty() && IsIcon(text.front())) text.remove_prefix(1);  // a Controls row's pad button
        }
        std::wstring clean = CleanForSpeech(text);
        if (!clean.empty()) row.pieces.push_back({r.layer, std::move(clean)});
    }
    return row;
}

// Left/Right change values (settings, option rows). Text that changes right after is the new value.
static std::atomic<ULONGLONG> g_valueKeyAt;
constexpr ULONGLONG kValueWindowMs = 500;

void MenuValueKeyPressed() { g_valueKeyAt = GetTickCount64(); }

void MenuAnyKeyPressed(bool gameKey) {
    g_anyKeyAt = GetTickCount64();
    if (gameKey) g_gameKeyAt = GetTickCount64();
}
ULONGLONG MenuLastGameKeyAt() { return g_gameKeyAt; }

void MenuSetCardInfo(const std::wstring& info) {
    std::lock_guard lock(g_mutex);
    g_cardInfo = info;
}

void MenuSuppressLive(const std::wstring& containing) {
    std::lock_guard lock(g_mutex);
    g_suppressLive = containing;
    g_suppressLiveAt = GetTickCount64();
}

void MenuExpectMessage() { g_messageAt = GetTickCount64(); }

void MenuSpokeElsewhere(const std::wstring& text) {
    std::lock_guard lock(g_mutex);
    g_elsewhere = text;
    g_elsewhereAt = GetTickCount64();
}

static bool ValueKeyRecent() { return GetTickCount64() - g_valueKeyAt < kValueWindowMs; }

static void OnTextChanged(const void* layer, std::wstring_view text, float x, float y, bool firstSeen, bool moved) {
    // Text opening with an icon is a key prompt ("[Enter] OK") unless it's a label of the focused widget (a shop
    // pack's "[cards] 85/314"); prompts are never values, descriptions or news. Several lines are a message
    // ("[Up] is the only key assigned to|Move Up").
    bool prompt = IsIcon(text.front()) && text.find_first_of(L"|\n") == std::wstring_view::npos;
    std::wstring clean = CleanForSpeech(text);
    if (clean.empty()) return;
    // A Controls row's keys changed: a key was assigned, or taken from this row by another.
    if (KeysOnly(text) && !firstSeen && !moved && !DuelInProgress()) {
        Focus row = RowFocus(y, nullptr);
        Log("keys changed: %s", ToUtf8(AllText(row)).c_str());
        if (!row.pieces.empty()) AnnounceItem(std::move(row));
        return;
    }
    // A duel's full card view (Show Details) has the card's name and type across the top, then its stats and text
    // as separate labels: say the card's full info, as I does.
    if (text.starts_with(L"@4") && y > 0.75f && DuelInProgress()) {
        std::wstring name = CleanForSpeech(text.substr(2, text.find(L'@', 2) - 2));
        std::wstring info;
        {
            std::lock_guard lock(g_mutex);
            info = g_cardInfo;
        }
        if (!info.starts_with(name)) info = clean;
        Focus view{{{layer, std::move(info)}}, {}, GetTickCount64()};
        view.card = true;
        AnnounceItem(std::move(view));
        return;
    }
    // A duel's card panels (left edge; a second one at the right during targets and materials) follow the cursor
    // and the engine; the cursor speech, the events and I cover them. The HUD's names and LP are in the corners.
    if ((x < -0.7f || x > 0.5f) && y > -0.7f && y < 0.7f && DuelInProgress()) return;
    // The HUD corners (names, LP counting down, the turn counter): LP changes are announced by the duel events.
    if ((x < -0.4f || x > 0.4f) && (y > 0.75f || y < -0.75f) && DuelInProgress()) return;
    if (!firstSeen) {
        bool sortLabel;
        {
            std::lock_guard lock(g_mutex);
            sortLabel = std::find(g_sortLayers.begin(), g_sortLayers.end(), layer) != g_sortLayers.end();
        }
        if (sortLabel) {
            AnnounceItem({{{layer, L"Sort by " + clean}}, {}, GetTickCount64()});
            return;
        }
    }
    // Left/Right also move through grids; when they moved focus, what changes is the new item's details.
    bool focusMoved = GetTickCount64() - g_lastFocusAt < kTipWindowMs;
    bool suppressed, cardShown;
    {
        std::lock_guard lock(g_mutex);
        suppressed = GetTickCount64() - g_suppressLiveAt < 1500 && !g_suppressLive.empty() && clean.starts_with(g_suppressLive);
        // A duel's cursor and card lists aren't widgets: the card they just showed is their focus move. A list to
        // pick from (Deck, Graveyard) relabels its cards' levels ("x4") as it scrolls or reopens; those aren't
        // values or new rows, even on a layer that was the list's OK button a moment ago.
        cardShown = GetTickCount64() - g_suppressLiveAt < kTipWindowMs;
    }
    focusMoved = focusMoved || cardShown;
    if (suppressed) return;  // a details panel repeating the card just announced (Left/Right in a duel's hand)
    if (!prompt && !firstSeen && ValueKeyRecent() && !focusMoved) {
        Log("value: %s", ToUtf8(clean).c_str());
        AnnounceItem({{{layer, std::move(clean)}}, {}, GetTickCount64()});
        return;
    }
    std::unique_lock lock(g_mutex);
    bool known = std::find(g_descriptionLayers.begin(), g_descriptionLayers.end(), layer) != g_descriptionLayers.end();
    // A label filled in just after its widget took focus belongs to that pending item (not the help bar's
    // description: the help bar can take focus too, but its text is a description).
    for (Focus& f : g_items) {
        if (known) break;
        if (prompt && f.cls && strstr(f.cls, "widget_Help")) continue;  // the help bar is the key prompt strip
        if (f.card) continue;  // a card's late badge belongs to its I info, not its name
        if (std::find(f.drawables.begin(), f.drawables.end(), layer) == f.drawables.end()) continue;
        if (std::none_of(f.pieces.begin(), f.pieces.end(), [&](const Focus::Piece& p) { return p.layer == layer; }))
            f.pieces.push_back({layer, std::move(clean)});
        g_wake.notify_one();
        return;
    }
    if (prompt) return;
    ULONGLONG now = GetTickCount64();
    bool focusedRow = std::find(g_focusLayers.begin(), g_focusLayers.end(), layer) != g_focusLayers.end();
    // The row's other labels (its number after its name) change in the same frame.
    auto sameRow = std::find_if(g_items.begin(), g_items.end(), [&](const Focus& f) { return f.relabel && now - f.at <= kCoalesceMs; });
    // A game key since the last focus change means this is a new row, however fast the presses come; otherwise
    // wait out labels still being filled in for that focus.
    if (focusedRow && !firstSeen && !cardShown &&
        (sameRow != g_items.end() || now - g_lastFocusAt > kTipWindowMs || g_gameKeyAt > g_lastFocusAt)) {
        if (now - g_elsewhereAt < 2000 && Contains(g_elsewhere, clean)) return;  // a phase banner reusing a button's layer
        // The cursor stayed on its row while the list scrolled under it: the row's new labels are the new item.
        Log("row relabelled: %s", ToUtf8(clean).c_str());
        if (sameRow != g_items.end()) {
            sameRow->pieces.push_back({layer, std::move(clean)});
        } else {
            // Not AnnounceItem: it would forget the row's other labels before they change too.
            Focus row{{{layer, std::move(clean)}}, {}, now};
            row.relabel = true;
            g_items.push_back(std::move(row));
            g_lastFocusAt = now;
        }
        g_wake.notify_one();
        return;
    }
    if (moved) {  // a scrolled label: not news, not a description
        // Two or more labels moving together is a scroll; mark it so the row coming into view is dropped too.
        // One alone is a submenu header taking over the chosen item's text: that must not drop the description.
        static ULONGLONG lastMovedAt;
        if (now - lastMovedAt < 50) g_lastScrollAt = now;
        lastMovedAt = now;
        return;
    }
    if (now - g_lastFocusAt > kTipWindowMs) {
        // No focus change behind it: story lines, messages. Collected briefly; spoken if the batch has prose.
        if (g_live.pieces.empty()) g_live = {{}, {}, now};
        // A duel prompt drawn as plain text ("Select an attack target monster.") is about where the cursor is.
        std::wstring cursor = DuelCursorText(clean);
        g_live.pieces.push_back({layer, std::move(clean)});
        bool hasCursor = std::any_of(g_live.pieces.begin(), g_live.pieces.end(), [](const Focus::Piece& p) { return p.layer == kAfterText; });
        if (!cursor.empty() && !hasCursor) g_live.pieces.push_back({kAfterText, std::move(cursor)});
        g_wake.notify_one();
        return;
    }
    if (std::find(g_focusLayers.begin(), g_focusLayers.end(), layer) != g_focusLayers.end()) return;
    // A submenu's header repeats the item chosen to open it; that is not a description.
    if (std::any_of(g_recentItems.begin(), g_recentItems.end(), [&](const std::wstring& item) { return Normalize(item) == Normalize(clean); })) return;

    bool atSlot = std::any_of(g_descriptionSlots.begin(), g_descriptionSlots.end(),
        [&](const auto& s) { return std::abs(s.first - x) < 0.02f && std::abs(s.second - y) < 0.02f; });
    // The duel signalled a message box: its line comes with the box's Ok button and is what the box says.
    if (now - g_messageAt < 1500) {
        if (std::wstring cursor = DuelCursorText(clean); !cursor.empty()) clean += L" " + cursor;
        AddTipLocked(layer, std::move(clean));
        return;
    }
    // New text at a new place is a screen being built, unless it reads like a description: a sentence or several
    // lines (the Duel Result tabs: "Tip: ...", "NEW CARD AWARD!\nTemple of the Kings"). List titles ("A Duel in
    // Love") have several words but no full stop.
    bool descriptive = clean.find(L'\n') != std::wstring::npos ||
                       (std::count(clean.begin(), clean.end(), L' ') >= 2 && wcschr(L".!?", clean.back()));
    if (firstSeen && !known && !atSlot && !descriptive) return;
    if (!atSlot && DuelInProgress()) return;  // a duel's card panels change with the cursor: they aren't descriptions
    if (!atSlot) {
        g_descriptionSlots.emplace_back(x, y);
        Log("description slot %.3f,%.3f", x, y);
    }
    AddTipLocked(layer, std::move(clean));
}

// A label drawn in the focus colour with no widget focus behind it (screens that highlight rows themselves):
// announce everything on its row, e.g. a setting's name and value.
static void OnHighlight(const void* layer, float, float y) {
    // A duel's dialogs put their buttons on one row (Yes, No) and reuse layers: only the lit button counts.
    bool duel = DuelInProgress();
    {
        std::lock_guard lock(g_mutex);
        if (!duel && std::find(g_focusLayers.begin(), g_focusLayers.end(), layer) != g_focusLayers.end()) return;  // widget focus has it
    }
    Focus row = RowFocus(y, layer);
    row.highlight = true;
    if (duel) std::erase_if(row.pieces, [&](const Focus::Piece& p) { return p.layer != layer; });
    Log("highlight %p: %s", layer, ToUtf8(AllText(row)).c_str());
    if (!row.pieces.empty()) AnnounceItem(std::move(row));
}

// The Settings volume bars are pictures, so the level is spoken from the call that applies it (channel 0 music,
// 1 sound effects, level 0-10). The game also calls it once at startup, which the key check filters out.
using SetVolumeFn = void(__fastcall*)(int channel, int level);
static SetVolumeFn g_setVolume;
static void __fastcall SetVolumeHook(int channel, int level) {
    g_setVolume(channel, level);
    if (!ValueKeyRecent()) return;
    Log("volume %d: %d", channel, level);
    AnnounceItem({{{nullptr, std::to_wstring(std::clamp(level, 0, 10))}}, {}, GetTickCount64()});
}

static bool Hook(void* target, void* detour, void** original, const char* name) {
    if (target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK) return true;
    Log("Could not hook %s", name);
    return false;
}

bool MenuHooksInstall() {
    void** widgetBase = FindVTable(".?AVwidget_Base@RIX@@");
    void* setDescription = FindInGame("widget_Help::SetDescription",
        "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 8B DA 48 8B 49 58 E8 ?? ?? ?? ?? 48 85 DB 74 ?? F3 0F 10 0D ?? ?? ?? ?? 41 B0 01");
    if (!widgetBase || !g_textLayerVTable) return false;

    std::thread(AnnouncerThread).detach();
    bool ok = Hook(widgetBase[1], (void*)&SetFocusedHook, (void**)&g_setFocused, "widget_Base::SetFocused");
    Hook(widgetBase[3], (void*)&SetEnabledHook, (void**)&g_setEnabled, "widget_Base::SetEnabled");
    // Reads the card data's name text (+0x18) through its Indx table (+0x28); its neighbour reads the card text.
    g_cardName = (CardNameFn)FindInGame("CardName",
        "48 8B 05 ?? ?? ?? ?? 4C 8B 40 18 48 8B 50 28 4D 85 C0 74 ?? 48 85 D2 74 ?? B8 3C 0F 00 00 66 2B C8");
    g_cardText = (CardNameFn)FindInGame("CardText",
        "48 8B 05 ?? ?? ?? ?? 4C 8B 40 38 48 8B 50 28 4D 85 C0 74 ?? 48 85 D2 74 ?? B8 3C 0F 00 00 66 2B C8");
    // In SetCard: "cmp [rbx+0x104],0; je; mov rcx,[rip+X]; movzx edx,[rbx+0x100]; mov rax,[rcx]; call [rax+8]".
    if (uint8_t* at = FindInGame("IsNewCard",
            "83 BB 04 01 00 00 00 74 ?? 48 8B 0D ?? ?? ?? ?? 0F B7 93 00 01 00 00 48 8B 01 FF 50 08"))
        g_collection = (void* const*)(at + 16 + *(const int32_t*)(at + 12));
    Hook(FindInGame("DeckStats::Refresh",
             "48 8B C4 41 56 48 81 EC A0 00 00 00 48 C7 44 24 20 FE FF FF FF 48 89 58 10 48 89 70 18 48 89 78 20 0F 29 70 E8 0F 29 78 D8"),
         (void*)&DeckStatsRefreshHook, (void**)&g_deckStatsRefresh, "DeckStats::Refresh");
    Hook(FindInGame("FilterOption::RefreshCheck",
             "40 53 48 83 EC 20 48 8B 81 00 01 00 00 48 8B D9 48 85 C0 74 ?? 8B 91 08 01 00 00 48 8D 0D"),
         (void*)&RefreshCheckHook, (void**)&g_refreshCheck, "FilterOption::RefreshCheck");
    Hook(FindInGame("SetCard", "40 53 48 83 EC 20 48 8B D9 66 39 91 00 01 00 00 0F 84"),
         (void*)&SetCardHook, (void**)&g_setCard, "widget_Card::SetCard");
    Hook(FindInGame("SetVolume", "48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8D 3D ?? ?? ?? ?? 48 63 D9 85 D2 7F 04 33 D2"),
         (void*)&SetVolumeHook, (void**)&g_setVolume, "SetVolume");
    g_onTextChanged = &OnTextChanged;
    g_onHighlight = &OnHighlight;
    // Without the description hook, focus still works; only tooltips are lost.
    Hook(setDescription, (void*)&SetDescriptionHook, (void**)&g_setDescription, "widget_Help::SetDescription");
    return ok;
}
