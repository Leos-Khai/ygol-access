"""Read card data from extracted bin\\ files and print it or export it as JSON.

Formats (each file is one record per internal card index; index 0 is an empty placeholder):
  CARD_Prop.bin      8 bytes: two little-endian u32.
                     first:  bits 0-13 card id, 14-22 ATK/10, 23-31 DEF/10 (511 = "?"; Link monsters store arrows in DEF)
                     second: bit 0 flag, 1-6 kind, 7-10 attribute, 11-14 level, 15-17 spell/trap icon,
                             18-22 type, 23-26 left scale, 27-30 right scale
  CARD_Indx_<L>.bin  per card: u32 offset into CARD_Name_<L>.bin, u32 offset into CARD_Desc_<L>.bin,
                     then one extra pair holding the two file sizes.
  CARD_Name/Desc     UTF-16LE strings, 0-terminated.
Bit layout cross-checked against Arefu/Yu-Gi-Oh-Ex (File Type Libraries/CARD_Props).

Usage: python tools/cards.py <extracted dir> [--lang E] [--find TEXT] [--json out.json]
"""
import argparse
import json
import os
import struct
import sys

KINDS = {
    0: "Normal", 1: "Effect", 2: "Fusion", 3: "Fusion Effect", 4: "Ritual", 5: "Ritual Effect",
    6: "Toon", 7: "Spirit", 8: "Union", 9: "Gemini", 10: "Token", 13: "Spell", 14: "Trap",
    15: "Tuner Normal", 16: "Tuner Effect", 17: "Synchro", 18: "Synchro Effect", 19: "Synchro Tuner Effect",
    22: "Xyz", 23: "Xyz Effect", 24: "Flip Effect", 25: "Pendulum Normal", 26: "Pendulum Effect",
    27: "Effect", 28: "Toon", 29: "Spirit", 30: "Tuner Effect", 32: "Tuner Flip Effect",
    33: "Pendulum Tuner Effect", 34: "Xyz Pendulum Effect", 35: "Pendulum Flip Effect",
    36: "Synchro Pendulum Effect", 37: "Union Tuner Effect", 38: "Ritual Spirit Effect", 39: "Fusion Tuner",
    40: "Pendulum Effect", 41: "Fusion Pendulum Effect", 42: "Link", 43: "Link Effect",
    44: "Pendulum Tuner Normal", 45: "Pendulum Spirit Effect",
}
ATTRIBUTES = {1: "LIGHT", 2: "DARK", 3: "WATER", 4: "FIRE", 5: "EARTH", 6: "WIND", 7: "DIVINE"}
TYPES = {
    1: "Dragon", 2: "Zombie", 3: "Fiend", 4: "Pyro", 5: "Sea Serpent", 6: "Rock", 7: "Machine", 8: "Fish",
    9: "Dinosaur", 10: "Insect", 11: "Beast", 12: "Beast-Warrior", 13: "Plant", 14: "Aqua", 15: "Warrior",
    16: "Winged Beast", 17: "Fairy", 18: "Spellcaster", 19: "Thunder", 20: "Reptile", 21: "Psychic",
    22: "Wyrm", 23: "Cyberse", 24: "Divine-Beast",
}
# Spell/Trap icons, checked against 20 known cards (Raigeki, Solemn Judgment, Umi, Axe of Despair, ...).
ICONS = {0: "Normal", 1: "Counter", 2: "Field", 3: "Equip", 4: "Continuous", 5: "Quick-Play", 6: "Ritual"}
# What the level field means, from bits 27-28 (low 2 bits of the right-scale field).
STAR_TYPES = {1: "Level", 2: "Rank", 3: "Link"}
LINK_ARROWS = ["Top-Left", "Top", "Top-Right", "Left", "Right", "Bottom-Left", "Bottom", "Bottom-Right"]


def read(dir_, name):
    with open(os.path.join(dir_, "bin", name), "rb") as f:
        return f.read()


def utf16z(data, offset):
    end = offset
    while end + 1 < len(data) and data[end:end + 2] != b"\0\0":
        end += 2
    return data[offset:end].decode("utf-16-le")


def stat(v):
    return None if v == 511 else v * 10


def load(dir_, lang="E"):
    prop = read(dir_, "CARD_Prop.bin")
    indx = read(dir_, f"CARD_Indx_{lang}.bin")
    names = read(dir_, f"CARD_Name_{lang}.bin")
    descs = read(dir_, f"CARD_Desc_{lang}.bin")
    count = len(prop) // 8
    assert len(indx) // 8 == count + 1, "Indx should have one more entry than Prop"
    assert struct.unpack_from("<II", indx, count * 8) == (len(names), len(descs)), "Indx end pair mismatch"

    cards = []
    for i in range(1, count):
        first, second = struct.unpack_from("<II", prop, i * 8)
        name_off, desc_off = struct.unpack_from("<II", indx, i * 8)
        kind = (second >> 1) & 63
        card = {
            "index": i,
            "id": first & 0x3FFF,
            "name": utf16z(names, name_off),
            "kind": KINDS.get(kind, f"kind {kind}"),
            "text": utf16z(descs, desc_off),
        }
        if kind == 13 or kind == 14:
            card["icon"] = ICONS.get((second >> 15) & 7)
        else:
            star = STAR_TYPES.get((second >> 27) & 3, "Level")
            card["attribute"] = ATTRIBUTES.get((second >> 7) & 15)
            card["type"] = TYPES.get((second >> 18) & 31)
            card[star.lower()] = (second >> 11) & 15
            card["atk"] = stat((first >> 14) & 511)
            if star == "Link":
                arrows = ((first >> 23) & 511)
                card["arrows"] = [a for b, a in enumerate(LINK_ARROWS) if arrows >> b & 1]
            else:
                card["def"] = stat((first >> 23) & 511)
            if "Pendulum" in card["kind"]:
                card["scale"] = (second >> 23) & 15
        cards.append(card)
    return cards


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("extracted_dir")
    ap.add_argument("--lang", default="E")
    ap.add_argument("--find", help="print cards whose name contains this text")
    ap.add_argument("--json", help="write all cards to this file")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")  # card text has symbols the Windows console codepage lacks

    cards = load(args.extracted_dir, args.lang)
    print(f"{len(cards)} cards")
    if args.find:
        for c in cards:
            if args.find.lower() in c["name"].lower():
                print(json.dumps(c, ensure_ascii=False))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(cards, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
