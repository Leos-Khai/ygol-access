"""Unpack YGO_2020.dat using its index YGO_2020.toc.

The .toc is text: a "UT" header line, then one line per file:
    <size hex> <name length hex> <path>
Files are stored in the .dat back to back in .toc order, each starting on a 4-byte boundary.

Usage: python tools/extract.py <game dir> <output dir> [--only SUBSTRING]
"""
import argparse
import os
import sys


def read_toc(toc_path):
    entries = []
    with open(toc_path, encoding="ascii") as f:
        if f.readline().strip() != "UT":
            sys.exit("Unexpected .toc header")
        for n, line in enumerate(f, start=2):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            size_hex, len_hex, path = line.split(maxsplit=2)
            if len(path) != int(len_hex, 16):
                sys.exit(f"toc line {n}: name length mismatch for {path!r}")
            entries.append((path, int(size_hex, 16)))
    return entries


def layout(entries):
    """Yields (path, offset, size)."""
    offset = 0
    for path, size in entries:
        yield path, offset, size
        offset = (offset + size + 3) & ~3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--only", help="extract only paths containing this text (case-insensitive)")
    args = ap.parse_args()

    entries = read_toc(os.path.join(args.game_dir, "YGO_2020.toc"))
    dat_path = os.path.join(args.game_dir, "YGO_2020.dat")
    files = list(layout(entries))
    _, last_off, last_size = files[-1]
    dat_size = os.path.getsize(dat_path)
    if not dat_size - 4 < last_off + last_size <= dat_size:
        sys.exit(f"Layout mismatch: entries end at {last_off + last_size}, .dat is {dat_size} bytes")

    out_root = os.path.realpath(args.out_dir)
    count = 0
    with open(dat_path, "rb") as dat:
        for path, offset, size in files:
            if args.only and args.only.lower() not in path.lower():
                continue
            dest = os.path.realpath(os.path.join(out_root, path.replace("\\", os.sep)))
            if not dest.startswith(out_root + os.sep):
                sys.exit(f"Refusing path outside output dir: {path!r}")
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            dat.seek(offset)
            with open(dest, "wb") as out:
                out.write(dat.read(size))
            count += 1
    print(f"Extracted {count} of {len(files)} files to {out_root}")


if __name__ == "__main__":
    main()
