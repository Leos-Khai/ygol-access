#include "cards.h"

#include <windows.h>

#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

void Log(const char* fmt, ...);

// Names for the CARD_Prop.bin fields, as in tools/cards.py (bit layout cross-checked against Arefu/Yu-Gi-Oh-Ex).
static const wchar_t* Kind(unsigned k) {
    static const wchar_t* const kinds[46] = {
        L"Normal", L"Effect", L"Fusion", L"Fusion Effect", L"Ritual", L"Ritual Effect", L"Toon", L"Spirit", L"Union",
        L"Gemini", L"Token", nullptr, nullptr, L"Spell", L"Trap", L"Tuner Normal", L"Tuner Effect", L"Synchro",
        L"Synchro Effect", L"Synchro Tuner Effect", nullptr, nullptr, L"Xyz", L"Xyz Effect", L"Flip Effect",
        L"Pendulum Normal", L"Pendulum Effect", L"Effect", L"Toon", L"Spirit", L"Tuner Effect", nullptr,
        L"Tuner Flip Effect", L"Pendulum Tuner Effect", L"Xyz Pendulum Effect", L"Pendulum Flip Effect",
        L"Synchro Pendulum Effect", L"Union Tuner Effect", L"Ritual Spirit Effect", L"Fusion Tuner",
        L"Pendulum Effect", L"Fusion Pendulum Effect", L"Link", L"Link Effect", L"Pendulum Tuner Normal",
        L"Pendulum Spirit Effect"};
    return k < 46 && kinds[k] ? kinds[k] : L"";
}
static const wchar_t* const kAttributes[] = {L"", L"LIGHT", L"DARK", L"WATER", L"FIRE", L"EARTH", L"WIND", L"DIVINE"};
static const wchar_t* const kTypes[] = {L"", L"Dragon", L"Zombie", L"Fiend", L"Pyro", L"Sea Serpent", L"Rock",
    L"Machine", L"Fish", L"Dinosaur", L"Insect", L"Beast", L"Beast-Warrior", L"Plant", L"Aqua", L"Warrior",
    L"Winged Beast", L"Fairy", L"Spellcaster", L"Thunder", L"Reptile", L"Psychic", L"Wyrm", L"Cyberse", L"Divine-Beast"};
static const wchar_t* const kIcons[] = {L"Normal", L"Counter", L"Field", L"Equip", L"Continuous", L"Quick-Play", L"Ritual"};
static const wchar_t* const kArrows[] = {L"top-left", L"top", L"top-right", L"left", L"right", L"bottom-left", L"bottom", L"bottom-right"};

static std::wstring Stat(unsigned v) { return v == 511 ? L"?" : std::to_wstring(v * 10); }

std::wstring DescribeProp(uint32_t first, uint32_t second) {
    unsigned kind = (second >> 1) & 63, attribute = (second >> 7) & 15, level = (second >> 11) & 15;
    unsigned icon = (second >> 15) & 7, type = (second >> 18) & 31, scale = (second >> 23) & 15;
    unsigned star = (second >> 27) & 3;  // 1 Level, 2 Rank, 3 Link rating
    unsigned atk = (first >> 14) & 511, def = (first >> 23) & 511;
    if (kind == 13 || kind == 14) return std::wstring(icon < 7 ? kIcons[icon] : L"") + (kind == 13 ? L" Spell" : L" Trap");

    std::wstringstream out;
    out << Kind(kind) << L" monster";
    if (attribute && attribute < 8) out << L", " << kAttributes[attribute];
    if (type && type < 25) out << (attribute ? L" " : L", ") << kTypes[type];
    if (star == 3) {
        out << L", Link " << level << L", ATK " << Stat(atk) << L", arrows";
        for (unsigned b = 0; b < 8; ++b) {
            if (def >> b & 1) out << L" " << kArrows[b];
        }
        return out.str();
    }
    if (level) out << (star == 2 ? L", Rank " : L", Level ") << level;
    if (std::wstring(Kind(kind)).find(L"Pendulum") != std::wstring::npos) out << L", Scale " << scale;
    out << L", ATK " << Stat(atk) << L", DEF " << Stat(def);
    return out.str();
}

// CARD_Prop.bin from YGO_2020.dat: the .toc lists "<size hex> <name length hex> <path>" per file, stored back to
// back in the .dat, each starting on a 4-byte boundary (tools/extract.py).
static std::vector<uint32_t> LoadPropFile() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir.resize(dir.find_last_of(L'\\') + 1);

    std::ifstream toc(dir + L"YGO_2020.toc");
    std::string line;
    std::getline(toc, line);  // "UT"
    uint64_t offset = 0;
    while (std::getline(toc, line)) {
        std::istringstream fields(line);
        std::string sizeHex, lenHex, path;
        if (!(fields >> sizeHex >> lenHex)) continue;
        std::getline(fields >> std::ws, path);
        uint64_t size = std::stoull(sizeHex, nullptr, 16);
        if (path == "bin\\CARD_Prop.bin") {
            std::ifstream dat(dir + L"YGO_2020.dat", std::ios::binary);
            std::vector<uint32_t> words(size / 4);
            dat.seekg((std::streamoff)offset);
            dat.read((char*)words.data(), (std::streamsize)(words.size() * 4));
            if (dat) return words;
            break;
        }
        offset = (offset + size + 3) & ~3ull;
    }
    Log("CARD_Prop.bin not found in YGO_2020.dat");
    return {};
}

std::wstring CardSummary(int16_t cardId) {
    static std::once_flag once;
    static std::unordered_map<int16_t, std::pair<uint32_t, uint32_t>> props;
    std::call_once(once, [] {
        std::vector<uint32_t> words = LoadPropFile();
        for (size_t i = 0; i + 1 < words.size(); i += 2) props[(int16_t)(words[i] & 0x3FFF)] = {words[i], words[i + 1]};
        Log("Card props: %zu cards", props.size());
    });
    auto it = props.find(cardId);
    return it == props.end() ? L"" : DescribeProp(it->second.first, it->second.second);
}
