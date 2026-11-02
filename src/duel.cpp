// Duel speech.
//
// Events: everything the duel screen shows for the rules engine goes through one function,
// Draw_DuelAnimationFromId(anim, a2, a3, a4) (0x1407C1450). Its ids and arguments are listed in Arefu's
// docs/DuelAnimations.md; the ones spoken here are handled in DuelAnimHook.
//
// Board: Duel::PlayerState (0x143497C40), one 0xD94-byte block per side. Block +0: LP XOR a 16-bit key
// (0x143330280). +0x0C: counts of hand, deck, graveyard, Extra Deck, banished. +0x4C: the 13 field zones in zone
// order (5 monster, 2 Extra Monster, 5 spell/trap, field), 24 bytes each: +0 card id in the low 14 bits, +6 bit 0
// defense position, +7 bit 0 face-up (0: set / face-down).
// +0x19C hand, +0x55C Extra Deck, +0x7B4 graveyard, +0xA0C banished: card ids in the low 14 bits of dwords.
// Base +0x3774: turn player; +0x3788: phase (0-5).
#include "duel.h"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cards.h"
#include "menu.h"
#include "rtti.h"
#include "scan.h"
#include "speech.h"
#include "text.h"

constexpr size_t kBlockSize = 0xD94, kCounts = 0x0C, kZones = 0x4C, kZoneSize = 0x18;
constexpr size_t kHand = 0x19C, kExtraDeck = 0x55C, kGraveyard = 0x7B4, kBanished = 0xA0C;
constexpr size_t kTurnPlayer = 0x3774, kPhase = 0x3788;
enum Count { kHandCount, kDeckCount, kGraveyardCount, kExtraCount, kBanishedCount };

static const uint8_t* g_state;     // Duel::PlayerState
static bool g_inDuel;
static bool g_dealt;  // "Duel start" was shown: draws from here on are announced
static int g_turn;
static int g_lp[2];
static std::vector<int64_t> g_marked;  // materials picked so far (side | zone << 1 | instance << 8), game thread
static ULONGLONG g_duelStartAt;  // "Duel start" (anim 1)
static ULONGLONG g_listAt;  // when a list to pick cards from last opened (anim 56)  // LP as the animations announce them (the state lags behind them)

static const wchar_t* const kPhases[] = {L"Draw Phase", L"Standby Phase", L"Main Phase 1", L"Battle Phase", L"Main Phase 2", L"End Phase"};

// ---- Board reading ------------------------------------------------------------------------------------

static const uint8_t* Block(int side) { return g_state + (side & 1) * kBlockSize; }
static int CountOf(int side, Count c) { return *(const int*)(Block(side) + kCounts + c * 4); }
static int16_t IdOf(uint32_t packed) { return (int16_t)(packed & 0x3FFF); }

static std::atomic<ULONGLONG> g_duelFrameAt;  // the duel's last frame (DuelDispatchHook)

static bool InDuel() {
    return g_state && g_inDuel && GetTickCount64() - g_duelFrameAt < 1000 &&
           CountOf(0, kDeckCount) + CountOf(0, kHandCount) + CountOf(1, kDeckCount) > 0;
}

bool DuelInProgress() { return InDuel(); }

static std::wstring Name(int16_t id) {
    std::wstring name = GameCardName(id);
    return name.empty() ? L"a card" : name;
}


// A field zone record's face and battle position (seen in duels: set cards have +7 = 0, tokens Summoned in defense
// +6 = 1).
static bool FaceDown(const uint8_t* zone) { return !(zone[7] & 1); }
static const wchar_t* PositionName(const uint8_t* zone) {
    bool defense = zone[6] & 1;
    if (FaceDown(zone)) return defense ? L"face-down defense" : L"face-down attack";
    return defense ? L"defense" : L"attack";
}
static const uint8_t* Zone(int side, int zone) { return Block(side) + kZones + zone * kZoneSize; }

// The engine's current ATK and DEF of a monster on the field, with equips, field spells and effects applied:
// Calc_EffectiveAtkDef_Modifiers(player, zone, out, 0, 0) (0x1400307C0, Arefu EffectSystem.md section 41) fills a
// 40-byte block, +4 ATK, +8 DEF. Found through Get_EffectiveAtk (0x140039B30), which calls it.
using CalcStatsFn = void(__fastcall*)(int player, int zone, void* out, int a4, int a5);
static CalcStatsFn g_calcStats;
static bool CurrentStats(int side, int zone, int& atk, int& def) {
    if (!g_calcStats) return false;
    __try {
        int out[16] = {};
        g_calcStats(side, zone, out, 0, 0);
        atk = out[1];
        def = out[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// "ATK 2300 (original 1800), DEF 300" for a monster on the field; the printed stats when the engine can't say.
// Link monsters have no DEF.
static std::wstring Stats(int side, int zone, int16_t id) {
    std::wstring summary = CardSummary(id);  // "... ATK 2500, DEF 2000"
    size_t atAtk = summary.find(L"ATK "), atDef = summary.find(L"DEF ");
    int baseAtk = atAtk != std::wstring::npos ? _wtoi(summary.c_str() + atAtk + 4) : -1;
    int baseDef = atDef != std::wstring::npos ? _wtoi(summary.c_str() + atDef + 4) : -1;
    int atk, def;
    if (!CurrentStats(side, zone, atk, def)) return atAtk != std::wstring::npos ? summary.substr(atAtk) : L"";
    std::wstring out = L"ATK " + std::to_wstring(atk);
    if (baseAtk >= 0 && atk != baseAtk) out += L" (original " + std::to_wstring(baseAtk) + L")";
    if (baseDef >= 0) {
        out += L", DEF " + std::to_wstring(def);
        if (def != baseDef) out += L" (original " + std::to_wstring(baseDef) + L")";
    }
    return out;
}

// A zone's card: "Elemental HERO Neos, attack, ATK 2500 DEF 2000"; the opponent's face-down cards stay hidden.
static std::wstring ZoneCard(int side, const uint8_t* zone, bool monster) {
    int16_t id = IdOf(*(const uint32_t*)zone);
    if (id == 0) return L"";
    bool down = FaceDown(zone);
    if (side == 1 && down) return monster ? L"face-down monster" : L"set card";
    std::wstring out = Name(id);
    if (monster) {
        out += L", ";
        out += PositionName(zone);
        if (std::wstring stats = Stats(side, (int)((zone - Block(side) - kZones) / kZoneSize), id); !stats.empty())
            out += L", " + stats;
    } else if (down) {
        out += L", set";
    }
    return out;
}

static std::wstring Pile(int side, size_t offset, Count count, const wchar_t* what) {
    int n = CountOf(side, count);
    if (n <= 0) return std::wstring(L"No cards in ") + what;
    std::wstring out = std::to_wstring(n) + (n == 1 ? L" card in " : L" cards in ") + what;
    const uint32_t* ids = (const uint32_t*)(Block(side) + offset);
    for (int i = n - 1; i >= 0 && i < 226; --i) out += L", " + Name(IdOf(ids[i]));  // top (last added) first
    return out;
}

static void Say(const std::wstring& text, bool interrupt) {
    Log("duel say: %s", ToUtf8(text).c_str());
    Speak(ToUtf8(text).c_str(), interrupt);
}

void DuelSpeakField(int side) {
    if (!InDuel()) return Say(L"Not in a duel", true);
    std::wstring out = side ? L"Opponent's field. " : L"Your field. ";
    std::wstring monsters, spells;
    for (int i = 0; i < 5; ++i) {
        std::wstring m = ZoneCard(side, Zone(side, i), true);
        if (!m.empty()) monsters += L"Monster zone " + std::to_wstring(i + 1) + L": " + m + L". ";
        std::wstring s = ZoneCard(side, Zone(side, 7 + i), false);
        if (!s.empty()) spells += L"Spell and trap zone " + std::to_wstring(i + 1) + L": " + s + L". ";
    }
    for (int i = 0; i < 2; ++i) {
        std::wstring m = ZoneCard(side, Zone(side, 5 + i), true);
        if (!m.empty()) monsters += L"Extra Monster Zone " + std::to_wstring(i + 1) + L": " + m + L". ";
    }
    std::wstring field = ZoneCard(side, Zone(side, 12), false);
    out += monsters.empty() ? L"No monsters. " : monsters;
    out += spells.empty() ? L"No spells or traps. " : spells;
    if (!field.empty()) out += L"Field spell: " + field + L".";
    Say(out, true);
}

void DuelSpeakHand(int side) {
    if (!InDuel()) return Say(L"Not in a duel", true);
    int n = CountOf(side, kHandCount);
    if (side == 1) return Say(L"Opponent has " + std::to_wstring(n) + (n == 1 ? L" card in hand" : L" cards in hand"), true);
    std::wstring out = std::to_wstring(n) + (n == 1 ? L" card in hand" : L" cards in hand");
    const uint32_t* ids = (const uint32_t*)(Block(0) + kHand);
    for (int i = 0; i < n && i < 120; ++i) out += L", " + Name(IdOf(ids[i]));
    Say(out, true);
}

void DuelSpeakGraveyard(int side) {
    if (!InDuel()) return Say(L"Not in a duel", true);
    Say(std::wstring(side ? L"Opponent: " : L"") + Pile(side, kGraveyard, kGraveyardCount, L"the Graveyard"), true);
}

void DuelSpeakBanished(int side) {
    if (!InDuel()) return Say(L"Not in a duel", true);
    Say(std::wstring(side ? L"Opponent: " : L"") + Pile(side, kBanished, kBanishedCount, L"banishment"), true);
}

void DuelSpeakExtraDeck(int side) {
    if (!InDuel()) return Say(L"Not in a duel", true);
    if (side == 1) {
        int n = CountOf(1, kExtraCount);
        return Say(L"Opponent has " + std::to_wstring(n) + L" cards in the Extra Deck", true);
    }
    Say(Pile(0, kExtraDeck, kExtraCount, L"the Extra Deck"), true);
}

void DuelSpeakTurn() {
    if (!InDuel()) return Say(L"Not in a duel", true);
    int player = *(const int*)(g_state + kTurnPlayer), phase = *(const int*)(g_state + kPhase);
    std::wstring out = L"Turn " + std::to_wstring(g_turn) + L", " + (player ? L"opponent's " : L"your ");
    out += phase >= 0 && phase < 6 ? kPhases[phase] : L"turn";
    out += L". Deck: you " + std::to_wstring(CountOf(0, kDeckCount)) + L", opponent " + std::to_wstring(CountOf(1, kDeckCount));
    Say(out, true);
}

// ---- Events ----------------------------------------------------------------------------------------------

// Locations in animation arguments are packed: side | zone << 1 | index << 6 (| 0x4000 for a card reference).
// Zones: 0-4 monster, 5-6 Extra Monster, 7-11 spell/trap, 12 field, 13 hand, 14 Extra Deck, 15 Deck,
// 16 Graveyard, 17 banished.
struct Loc {
    int side, zone, index;
};
static Loc Unpack(uint32_t ref) { return {(int)(ref & 1), (int)((ref >> 1) & 0x1F), (int)((ref >> 6) & 0xFF)}; }

static const uint8_t* FieldSlot(Loc l) { return l.zone >= 0 && l.zone <= 12 ? Zone(l.side, l.zone) : nullptr; }

static int16_t CardAt(Loc l) {
    if (const uint8_t* slot = FieldSlot(l)) return IdOf(*(const uint32_t*)slot);
    const uint8_t* block = Block(l.side);
    size_t pile = l.zone == 13 ? kHand : l.zone == 14 ? kExtraDeck : l.zone == 16 ? kGraveyard : l.zone == 17 ? kBanished : 0;
    if (!pile || l.index >= 120) return 0;
    return IdOf(((const uint32_t*)(block + pile))[l.index]);
}
static int16_t CardAt(uint32_t ref) { return CardAt(Unpack(ref)); }

// Whether you may know the card at a location: not the Deck, and not the opponent's hand, Extra Deck or
// face-down cards.
static bool Known(Loc l) {
    if (l.zone == 15) return false;
    if (l.side == 0) return true;
    if (l.zone == 13 || l.zone == 14) return false;
    const uint8_t* slot = FieldSlot(l);
    return !slot || !FaceDown(slot);
}

static std::wstring Place(Loc l) {
    switch (l.zone) {
    case 5: case 6: return L"Extra Monster Zone " + std::to_wstring(l.zone - 4);
    case 12: return L"field zone";
    case 13: return L"hand";
    case 14: return L"Extra Deck";
    case 15: return L"Deck";
    case 16: return L"Graveyard";
    case 17: return L"banishment";
    }
    if (l.zone <= 4) return L"monster zone " + std::to_wstring(l.zone + 1);
    if (l.zone <= 11) return L"spell and trap zone " + std::to_wstring(l.zone - 6);
    return L"zone " + std::to_wstring(l.zone);
}

// "your monster zone 2", "the opponent's Graveyard"
static std::wstring Where(Loc l) { return (l.side ? L"the opponent's " : L"your ") + Place(l); }

static std::wstring CardName(Loc l, const wchar_t* hidden = L"a card") {
    int16_t id = CardAt(l);
    return Known(l) && id ? Name(id) : hidden;
}

// Subject and verb for a side: "You activate" / "Opponent activates".
static std::wstring Does(int side, const wchar_t* verb) { return std::wstring(side & 1 ? L"Opponent " : L"You ") + verb + (side & 1 ? L"s" : L""); }

static const uint16_t* g_cursor;  // the duel cursor's location (see CursorMovedHook)

// A card's instance number (unique per duel) from its pile or zone word, as the engine's lookup 0x140044480 reads it.
static int InstanceOf(uint32_t word) { return (int)((word >> 14) & 1) + (int)((word >> 23) & 0xFF) * 2; }

// A material's name from (side, zone, instance): a field zone holds one card; in the hand or a pile, the card with
// that instance (the low 16 bits of the event's a4).
static std::wstring MaterialName(int side, int zone, int instance) {
    if (zone <= 12) return CardName(Loc{side, zone, 0}) + L" in " + Where(Loc{side, zone, 0});  // two may share a name
    Count counts[] = {kHandCount, kExtraCount, kDeckCount, kGraveyardCount, kBanishedCount};
    size_t piles[] = {kHand, kExtraDeck, 0, kGraveyard, kBanished};
    if (zone >= 13 && zone <= 17 && piles[zone - 13]) {
        const uint32_t* words = (const uint32_t*)(Block(side) + piles[zone - 13]);
        for (int i = 0, n = CountOf(side, counts[zone - 13]); i < n && i < 120; ++i)
            if (InstanceOf(words[i]) == instance) return CardName(Loc{side, zone, i});
    }
    return std::wstring(L"A card from ") + (side ? L"the opponent's " : L"your ") + Place(Loc{side, zone, 0});
}

using DuelAnimFn = int(__fastcall*)(int anim, int a2, int a3, int a4);
static DuelAnimFn g_duelAnim;

static int __fastcall DuelAnimHook(int anim, int a2, int a3, int a4) {
    if (anim != 4) Log("duel anim %d (%d, 0x%X, 0x%X, 0x%X)", anim, a2, (unsigned)a2, (unsigned)a3, (unsigned)a4);
    std::wstring say;
    switch (anim) {
    case 1:  // DuelStart
        g_inDuel = g_dealt = true;
        g_duelStartAt = GetTickCount64();
        g_turn = 0;
        say = L"Duel start";
        break;
    case 2:  // DuelEnd: 1 local won, 2 other won, 3 draw
        say = a2 == 1 ? L"You win the duel" : a2 == 2 ? L"You lose the duel" : L"The duel is a draw";
        g_inDuel = g_dealt = false;
        break;
    case 5:  // PhaseChange: side, phase. The engine sends no TurnStart for the first turn: turns count at Draw Phases.
        g_inDuel = true;
        if (a3 == 0) say = L"Turn " + std::to_wstring(++g_turn) + L", " + (a2 & 1 ? L"opponent's turn. " : L"your turn. ");
        if (a3 >= 0 && a3 < 6) say += std::wstring(a2 & 1 ? L"Opponent's " : L"Your ") + kPhases[a3];
        break;
    case 30: {  // CardSet: side, zone
        Loc l{a2 & 1, a3, 0};
        if (a3 < 0 || a3 > 12) break;
        // A set card is face-down: only your own is named.
        std::wstring card = (a2 & 1) ? (l.zone < 5 ? L"a monster" : L"a card") : CardName(l);
        say = Does(a2, L"set") + L" " + card + L" in " + Place(l);
        break;
    }
    case 26: {  // CardMove: card | kind << 16, from, to
        if (!g_dealt) break;  // the opening hands are dealt before "Duel start"
        Loc from = Unpack((uint32_t)a3), to = Unpack((uint32_t)a4);
        if (from.zone == 15 && to.zone == 13 && (a2 >> 16) == 9) {  // a draw (kind 9; 0xB is an effect's search)
            say = to.side ? L"Opponent draws a card" : L"You draw " + CardName(to);
        } else if (from.zone == 15 && to.zone == 13) {  // added by an effect: the card is shown to both players
            say = (to.side ? L"Opponent adds " : L"You add ") + Name(CardAt(to)) + L" from the Deck to the hand";
        } else if (from.zone == 13 && to.zone <= 12) {
            // Hand to field: the summon, set or activation that follows names the card and the zone.
        } else if (to.zone > 17) {
            // Nowhere: a token leaving the field. It is gone already, so it can't be named.
            say = (from.side ? L"Opponent's token in " : L"Your token in ") + Place(from) + L" leaves the field";
        } else {
            // The card is named from where it went (where it was is empty by now), if you may know it there.
            int16_t id = Known(to) ? CardAt(to) : 0;
            say = (from.side ? L"Opponent's " : L"Your ") + (id ? Name(id) : L"card") + L" goes from " + Place(from) +
                  L" to " + (to.side != from.side ? Where(to) : Place(to));
        }
        break;
    }
    case 19: {  // HandCardReveal: side, hand index
        Loc l{a2 & 1, 13, a3};
        int16_t id = CardAt(l);
        say = Does(a2, L"reveal") + L" " + (id ? Name(id) : L"a card") + L" from hand";
        break;
    }
    case 60:  // ShowCardInfo: card id
        if (a2 > 0) say = L"Shown: " + Name((int16_t)a2);
        break;
    case 29:  // CardChangeId: packed, card id, instance
        if (Name(CardAt((uint32_t)a2)) != Name((int16_t)a3))  // a token taking its place says nothing new
            say = Name(CardAt((uint32_t)a2)) + L" becomes " + Name((int16_t)a3);
        break;
    case 36:  // CardNegate: card, a3, instance
        say = CardName(Unpack((uint32_t)a2)) + L" is negated";
        break;
    case 38: {  // CounterChange: side | zone << 8, count, instance
        Loc l{a2 & 1, (a2 >> 8) & 0xFF, 0};
        say = CardName(l) + L" in " + Where(l) + L" now has " + std::to_wstring(a3) + (a3 == 1 ? L" counter" : L" counters");
        break;
    }
    case 25: {  // CursorSelect: side, zone | index << 8 (a card being picked, e.g. as a target)
        Loc l{a2 & 1, a3 & 0xFF, (a3 >> 8) & 0xFF};
        say = L"Selected: " + CardName(l) + L" in " + Where(l);
        break;
    }
    case 27:  // CardSwap: card A, card B
        say = CardName(Unpack((uint32_t)a2)) + L" and " + CardName(Unpack((uint32_t)a3)) + L" switch places";
        break;
    case 44: case 47: case 50: case 83: case 88:  // material Clear: the picks are undone
        g_marked.clear();
        break;
    case 43: case 46: case 49: case 82: case 87:    // material Mark (picked): side, zone, instance
    case 45: case 48: case 51: case 84: case 89: {  // material Play (used): the same
        bool mark = anim == 43 || anim == 46 || anim == 49 || anim == 82 || anim == 87;
        // Marks are re-sent while the summon plays out: each picked card is said once, until a Clear or Play.
        std::vector<int64_t>& marked = g_marked;
        int64_t markKey = (int64_t)(a2 & 1) | (int64_t)a3 << 1 | (int64_t)(uint32_t)a4 << 8;
        if (mark && std::find(marked.begin(), marked.end(), markKey) != marked.end()) break;
        if (mark) marked.push_back(markKey);
        else marked.clear();
        int kind = anim == 43 || anim == 45 ? 0 : anim <= 48 ? 1 : anim <= 51 ? 2 : anim <= 84 ? 3 : 4;
        static const wchar_t* const kKinds[] = {L"Tribute", L"Fusion Material", L"Synchro Material", L"Xyz Material", L"Link Material"};
        std::wstring card = MaterialName(a2 & 1, a3, a4 & 0xFFFF);
        if (mark) say = card + L" selected as " + kKinds[kind];
        else say = kind == 0 ? card + L" is tributed" : card + L" is used as " + kKinds[kind];
        break;
    }
    case 61:  // Coin: card ref, card id, bit 0 result
        say = std::wstring(L"Coin toss: ") + ((a4 & 1) ? L"heads" : L"tails");
        break;
    case 62:  // Dice: side, roll
        say = Does(a2, L"roll") + L" a " + std::to_wstring(a3);
        break;
    case 21:  // DeckShuffle: side
        say = (a2 & 1) ? L"Opponent's Deck is shuffled" : L"Your Deck is shuffled";
        break;
    case 28: {  // Face or position change: card, a3 (Arefu calls it CardReturn; seen for Kunai with Chain turning an
                // attacker to defense and for set cards flipping to be activated). The record already has the result.
        // a3 1: flipped face-up (the record still says face-down; spell/trap flips are followed by their activation);
        // 0: battle position changed (the record has the new one).
        Loc l = Unpack((uint32_t)a2);
        const uint8_t* slot = FieldSlot(l);
        if (!slot || l.zone > 6) break;
        if (a3 == 1) say = Name(CardAt(l)) + L" in " + Where(l) + L" is flipped face-up";
        else say = CardName(l, L"A face-down monster") + L" in " + Where(l) + L" is now in " + PositionName(slot) + L" position";
        break;
    }
    case 15:  // LP_Set: side, LP (8000 at the start)
        g_lp[a2 & 1] = a3;
        break;
    case 16:  // LP_Change: side, delta, a4 0x101 then 0x100 for the same change. Sent before the LP is applied.
        if (a4 & 1) {
            g_lp[a2 & 1] += a3;
            say = Does(a2, a3 < 0 ? L"lose" : L"gain") + L" " + std::to_wstring(std::abs(a3)) +
                  L" LP, now " + std::to_wstring(g_lp[a2 & 1] > 0 ? g_lp[a2 & 1] : 0);
        }
        break;
    case 35: {  // CardActivate: card, a3, card id. Which card and from where (a zone, the hand, the Graveyard).
        Loc l = Unpack((uint32_t)a2);
        int16_t id = (int16_t)(a3 & 0x3FFF);  // a3 is the card id, a4 its location again
        if (id <= 0) id = CardAt(l);
        say = Does(l.side, L"activate") + L" " + Name(id) + (l.zone <= 12 ? L" in " : L" from ") + Place(l);
        break;
    }
    case 57: {  // SummonNormal: packed, a3 (not the card id; the summoned card is face-up in its zone by now)
        Loc l = Unpack((uint32_t)a2);
        say = Does(l.side, L"summon") + L" " + CardName(l) + L" in " + Place(l);
        break;
    }
    case 58: {  // SummonSpecial: packed, instance
        Loc l = Unpack((uint32_t)a2);
        say = Does(l.side, L"special summon") + L" " + CardName(l) + L" in " + Place(l);
        break;
    }
    case 12:  // AttackDeclare: attacker side | zone << 8, target side, target zone (< 0 cancelled, >= 7 direct)
        if (a4 >= 0 && g_state) {
            Loc attacker{a2 & 0xFF & 1, (a2 >> 8) & 0xFF, 0}, target{a3 & 1, a4, 0};
            say = CardName(attacker, L"A monster") + L" in " + Where(attacker) +
                  (a4 >= 7 ? std::wstring(L" attacks directly") : L" attacks " + CardName(target, L"a face-down monster") + L" in " + Where(target));
        }
        break;
    case 53:  // ChainResolve: side, a3, chain length in the low 16 bits
        if ((a4 & 0xFFFF) > 1) say = L"Chain of " + std::to_wstring(a4 & 0xFFFF) + L" resolves";
        break;
    case 6:  // TurnStart: the "Turn Change" banner; the Draw Phase announcement says the turn
        MenuSpokeElsewhere(L"Turn Change");
        break;
    case 56:  // SelectOption: a list or option to pick from opens
        g_listAt = GetTickCount64();
        break;
    case 55:  // Message: kind, a3, a4. (4, 1, 0) is sent constantly with nothing shown.
        if (a2 != 4 || a4 != 0) MenuExpectMessage();  // a box (Ok, or Yes/No) follows
        break;
    }
    int result = g_duelAnim(anim, a2, a3, a4);
    if (!say.empty()) {
        Log("duel say: %s", ToUtf8(say).c_str());
        MenuSpokeElsewhere(say);  // the phase banner's text then isn't read again as news
        Speak(ToUtf8(say).c_str(), false);  // queued: events follow each other
    }
    return result;
}

// Rock-paper-scissors (YGO_FRONT::Janken): the hands are pictures. Each player has a 0x24-byte record at
// +0xE00 + side * 0x24 whose int at +4 is the hand: 0 rock, 1 scissors, 2 paper (the order of the sprite name
// table at 0x140A573C0). The local hand is said when
// it changes. The choosing state is 5 (state at +0xE78), whose update is 0x1407B3CB0.
using JankenUpdateFn = void(__fastcall*)(uint8_t* janken);
static JankenUpdateFn g_jankenChoose;
static void __fastcall JankenChooseHook(uint8_t* janken) {
    g_jankenChoose(janken);
    static const uint8_t* lastJanken;  // game thread only
    static int lastHand = -1;
    int hand = *(const int*)(janken + 0xE00 + 4);
    if (janken == lastJanken && hand == lastHand) return;
    lastJanken = janken;
    lastHand = hand;
    static const wchar_t* const kHands[] = {L"Rock", L"Scissors", L"Paper"};
    if (hand >= 0 && hand < 3) Say(kHands[hand], true);
}

// What I reads for a card: name, summary, text.
static void SetCardInfo(int16_t id) {
    std::wstring info = Name(id);
    if (std::wstring summary = CardSummary(id); !summary.empty()) info += L". " + summary;
    if (std::wstring text = GameCardText(id); !text.empty()) info += L". " + text;
    MenuSetCardInfo(info);
    MenuSuppressLive(Name(id));  // the details panel's own text would repeat it
}

// A located card as the cursor names it: "Polymerization, your hand, 3 of 5", "Face-down monster, the opponent's
// monster zone 2", "Neos, attack, your monster zone 1".
static std::wstring DescribeCard(Loc l, int16_t id) {
    if (!Known(l)) {
        if (l.zone == 13) return L"Card " + std::to_wstring(l.index + 1) + L" of " + std::to_wstring(CountOf(l.side, kHandCount)) + L", " + Where(l);
        return std::wstring(l.zone < 5 ? L"Face-down monster" : L"Set card") + L", " + Where(l);
    }
    std::wstring say = Name(id);
    if (const uint8_t* slot = FieldSlot(l)) {
        if (l.zone <= 6) say += std::wstring(L", ") + PositionName(slot);
        else if (FaceDown(slot)) say += L", set";
    }
    say += L", " + Where(l);
    if (l.zone <= 6 && FieldSlot(l))
        if (std::wstring stats = Stats(l.side, l.zone, id); !stats.empty()) say += L", " + stats;
    if (l.zone == 13) say += L", " + std::to_wstring(l.index + 1) + L" of " + std::to_wstring(CountOf(l.side, kHandCount));
    return say;
}

// The last card or place the cursor and the details panel each spoke: the panel and the cursor can alternate every frame.
static uint32_t g_lastCursorKey = ~0u, g_lastPanelKey = ~0u;  // game thread only

// The duel cursor (the pointing hand): 0x14278EE78 holds where it is, packed like animation locations. Every move
// (keys, the engine pointing at a card) ends in 0x140773660, hooked here. Moves the player makes are spoken: the
// card, or the place and what is there (an empty zone, a pile's count).

// What is under the cursor, "Your Deck, 35 cards". id: the card there (0 for none or the Deck).
static std::wstring CursorText(Loc l, int16_t& id) {
    id = l.zone == 15 ? 0 : CardAt(l);
    std::wstring say;
    auto pile = [&](Count c) {
        int n = CountOf(l.side, c);
        say = Where(l) + L", " + (n ? std::to_wstring(n) + (n == 1 ? L" card" : L" cards") : L"empty");
        if (n && l.zone >= 16) {  // the Graveyard and banishment are face-up: name the top (last added) card
            id = CardAt(Loc{l.side, l.zone, n - 1});
            say += L", top " + Name(id);
        }
    };
    switch (l.zone) {
    case 14: pile(kExtraCount); break;
    case 15: pile(kDeckCount); break;
    case 16: pile(kGraveyardCount); break;
    case 17: pile(kBanishedCount); break;
    default:
        if (!id) say = Where(l) + L", empty";
        else say = DescribeCard(l, id);
    }
    say[0] = towupper(say[0]);  // "Your Deck", not "your Deck"
    return say;
}

std::wstring DuelCursorText(const std::wstring& prompt) {
    if (!g_cursor || !InDuel()) return L"";
    if (GetTickCount64() - g_listAt < 2000) return L"";  // a list to pick from is open: the field cursor isn't in use
    Loc l = Unpack(*g_cursor);
    if (l.zone > 17) return L"";
    int16_t id;
    std::wstring text = CursorText(l, id);
    // Only where the prompt is about the cursor's place: a zone to put a card in (an empty field zone), the card
    // it names ("Select [Elemental HERO Clayman] from your hand"), a card to activate or a target.
    bool emptyZone = l.zone <= 12 && !id && (prompt.find(L"position") != std::wstring::npos || prompt.find(L"zone") != std::wstring::npos);
    bool named = id && prompt.find(Name(id)) != std::wstring::npos;
    bool activate = prompt.find(L"to activate") != std::wstring::npos || prompt.find(L"target") != std::wstring::npos;
    return emptyZone || named || activate ? text : L"";
}

using CursorMovedFn = void(__fastcall*)();
static CursorMovedFn g_cursorMoved;
static void __fastcall CursorMovedHook() {
    g_cursorMoved();
    // Only moves from a key the player pressed in the duel: the engine points on its own (see ShowCardHook), and the
    // cursor appears at the start right after the key that closed the intro.
    ULONGLONG keyAt = MenuLastGameKeyAt();
    if (!InDuel() || GetTickCount64() - keyAt > 1000 || keyAt < g_duelStartAt) return;
    Loc l = Unpack(*g_cursor);
    if (l.zone > 17) return;
    int16_t id;
    std::wstring say = CursorText(l, id);
    uint32_t key = (uint32_t)*g_cursor << 16 | (uint16_t)id;
    if (key == g_lastCursorKey) return;
    g_lastCursorKey = key;
    if (id && Known(l)) SetCardInfo(id);
    Say(say, true);
}

// The card details panel (widget_CardDetailsDuel) is given a card through 0x1408866B0(widget, info): info +6 the
// card id (-1 none), +8 its location packed as in the animations (zone 18: no location, e.g. a card in a list).
// This covers cards shown away from the player's cursor: the engine pointing at what the opponent uses, and lists
// to pick from.
using ShowCardFn = void(__fastcall*)(void* widget, const uint8_t* info);
static ShowCardFn g_showCard;
static void __fastcall ShowCardHook(void* widget, const uint8_t* info) {
    g_showCard(widget, info);
    uint16_t ref = *(const uint16_t*)(info + 8);
    int16_t id = *(const int16_t*)(info + 6);
    uint32_t key = (uint32_t)ref << 16 | (uint16_t)id;
    if (key == g_lastPanelKey || id <= 0) return;
    g_lastPanelKey = key;
    Loc l = Unpack(ref);
    bool located = l.zone <= 17 && g_state;
    bool onField = located && l.zone <= 12;
    // The panel shows the card's face, so it is known, except a face-down card on the field (the panel shows it to
    // its owner only).
    if (!onField || Known(l)) SetCardInfo(id);
    // A card on the field is the cursor's or an event's to say (the engine points at what is activated, attacks or
    // is picked as material). Spoken here: cards in lists to pick from (from a pile, the Deck, the Extra Deck) as
    // the player moves through them or as the list opens (anim 56), and cards with no place (a token being made).
    ULONGLONG now = GetTickCount64();
    bool asked = now - MenuLastGameKeyAt() < 1000 || now - g_listAt < 2000;
    bool atCursor = g_cursor && (ref & 0x3FFF) == (*g_cursor & 0x3FFF);  // 0x4000: card ref flag
    // The hand is the cursor's too: the panel re-shows hand cards as they are picked as materials.
    if (onField || l.zone == 13 || (located && (!asked || atCursor))) return;
    Say(located ? Name(id) + L", " + Where(l) : Name(id), true);
}

// "Use which effect of X?" (YGO_FRONT::DUEL_DIALOG_SELECT_EFFECT): tabs "Choice 1".."Choice N" over one description
// box, and a "not allowed" picture on choices that can't be used. Its setup, 0x14077B280(dialog, a2, a3), leaves
// the tab list at +0x258/+0x260 (0x70 bytes each), a can't-use flag per choice at +0x2F8 + i, and each choice's
// description as a std::wstring at +0x300 + i * 0x20. The spoken description gets the choice's number and
// availability from these.
using EffectDialogSetupFn = void(__fastcall*)(uint8_t* dialog, void* a2, char a3);
static EffectDialogSetupFn g_effectDialogSetup;
static std::atomic<uint8_t*> g_effectDialog;
static void** g_effectDialogVTable;
static void __fastcall EffectDialogSetupHook(uint8_t* dialog, void* a2, char a3) {
    g_effectDialogSetup(dialog, a2, a3);
    g_effectDialog = dialog;
}

// The choices' descriptions and flags, read under SEH: the dialog may be closed and freed meanwhile.
struct EffectChoices {
    int count = 0;
    bool blocked[8];
    wchar_t text[8][256];
};
static bool ReadEffectChoices(const uint8_t* dialog, EffectChoices& out) {
    __try {
        if (*(void* const*)dialog != g_effectDialogVTable) return false;
        int64_t count = (*(const int64_t*)(dialog + 0x260) - *(const int64_t*)(dialog + 0x258)) / 0x70;
        if (count <= 0 || count > 8) return false;
        out.count = (int)count;
        for (int i = 0; i < out.count; ++i) {
            out.blocked[i] = dialog[0x2F8 + i] != 0;
            const uint8_t* s = dialog + 0x300 + i * 0x20;  // MSVC std::wstring: buffer or pointer, size, capacity
            size_t size = *(const size_t*)(s + 0x10), capacity = *(const size_t*)(s + 0x18);
            const wchar_t* chars = capacity > 7 ? *(const wchar_t* const*)s : (const wchar_t*)s;
            size_t n = size < 255 ? size : 255;
            memcpy(out.text[i], chars, n * sizeof(wchar_t));
            out.text[i][n] = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring DuelAnnotate(const std::wstring& text) {
    uint8_t* dialog = g_effectDialog;
    EffectChoices choices;
    if (!dialog || !g_effectDialogVTable || !ReadEffectChoices(dialog, choices)) return text;
    for (int i = 0; i < choices.count; ++i) {
        std::wstring description = CleanForSpeech(choices.text[i]);
        size_t at = description.empty() ? std::wstring::npos : text.find(description);
        if (at == std::wstring::npos) continue;
        std::wstring label = L"Choice " + std::to_wstring(i + 1) + L" of " + std::to_wstring(choices.count) +
                             (choices.blocked[i] ? L", not available: " : L": ");
        return text.substr(0, at) + label + text.substr(at);
    }
    return text;
}

// Leaving a duel without a result (Pause menu > Exit Game) sends no DuelEnd, and the duel's objects outlive it.
// ExGameDuel::OnDispatch (vtable slot 5, 0x1407EBA30) runs every frame of a duel: a duel whose dispatch stopped
// is over.
using DuelDispatchFn = void(__fastcall*)(void* game, double dt);
static DuelDispatchFn g_duelDispatch;
static void __fastcall DuelDispatchHook(void* game, double dt) {
    g_duelFrameAt = GetTickCount64();
    g_duelDispatch(game, dt);
}

bool DuelHooksInstall() {
    if (uint8_t* get = FindInGame("Get_EffectiveAtk",
            "48 83 EC 68 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 45 33 C9 C7 44 24 20 00 00 00 00 4C 8D 44 24 30 "
            "E8 ?? ?? ?? ?? 8B 44 24 34"))
        g_calcStats = (CalcStatsFn)(get + 0x23 + 5 + *(const int32_t*)(get + 0x24));  // its call
    if (void** vt = FindVTable(".?AVExGameDuel@@");
        vt && MH_CreateHook(vt[5], (void*)&DuelDispatchHook, (void**)&g_duelDispatch) == MH_OK)
        MH_EnableHook(vt[5]);
    else Log("Could not hook ExGameDuel::OnDispatch");
    g_effectDialogVTable = FindVTable(".?AVDUEL_DIALOG_SELECT_EFFECT@YGO_FRONT@@");
    if (!g_effectDialogVTable) g_effectDialogVTable = FindVTable(".?AUDUEL_DIALOG_SELECT_EFFECT@YGO_FRONT@@");
    if (!g_effectDialogVTable) Log("No vtable for DUEL_DIALOG_SELECT_EFFECT");
    if (void* setup = FindInGame("DUEL_DIALOG_SELECT_EFFECT::Setup",
            "40 55 53 56 57 41 55 41 57 48 8D AC 24 68 FF FF FF 48 81 EC 98 01 00 00 0F 29 B4 24 80 01 00 00 48 8B 05 "
            "?? ?? ?? ?? 48 33 C4 48 89 45 50 0F 57 F6 44 88 44 24 51");
        setup && MH_CreateHook(setup, (void*)&EffectDialogSetupHook, (void**)&g_effectDialogSetup) == MH_OK)
        MH_EnableHook(setup);
    void* show = FindInGame("CardDetailsDuel::ShowCard",
        "40 55 56 57 41 56 41 57 48 83 EC 60 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 A0 00 00 00 0F 29 74 24 50");
    if (show && MH_CreateHook(show, (void*)&ShowCardHook, (void**)&g_showCard) == MH_OK) MH_EnableHook(show);
    if (uint8_t* moved = FindInGame("DuelCursor::Moved",
            "40 57 48 83 EC 60 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 70 0F 29 74 24 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
            "48 89 44 24 40 48 8D 54 24 38 48 8D 0D ?? ?? ?? ?? E8")) {
        g_cursor = (const uint16_t*)(moved + 0x2D + 7 + *(const int32_t*)(moved + 0x2D + 3));  // lea rcx, cursor
        if (MH_CreateHook(moved, (void*)&CursorMovedHook, (void**)&g_cursorMoved) == MH_OK) MH_EnableHook(moved);
    }
    void* choose = FindInGame("Janken::Choose", "4C 8B DC 57 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 50 80 B9 90 0E 00 00 00");
    if (choose && MH_CreateHook(choose, (void*)&JankenChooseHook, (void**)&g_jankenChoose) == MH_OK) MH_EnableHook(choose);
    if (uint8_t* at = FindInGame("PlayerState",
            "48 8D 2D ?? ?? ?? ?? 0F 1F 00 8B C6 83 E0 01 48 69 C8 94 0D 00 00 0F B7 05 ?? ?? ?? ??")) {
        g_state = at + 7 + *(const int32_t*)(at + 3);
    }
    void* anim = FindInGame("Draw_DuelAnimationFromId",
        "40 53 55 56 57 41 56 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30 8B 05 ?? ?? ?? ??");
    if (!anim || MH_CreateHook(anim, (void*)&DuelAnimHook, (void**)&g_duelAnim) != MH_OK || MH_EnableHook(anim) != MH_OK) {
        Log("Could not hook Draw_DuelAnimationFromId");
        return false;
    }
    return g_state != nullptr;
}
