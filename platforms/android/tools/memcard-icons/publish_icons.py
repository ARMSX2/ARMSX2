#!/usr/bin/env python3
"""Write the files the app downloads: every icon build_index.py chose, each compressed on its own
with zstd, and the serial index. Also bundles them into one zip (stored, not deflated: the files
inside are compressed already) for whoever uploads them.

Published layout, under the host's base URL:
    index.txt.zst      "SERIAL HASH" lines, as build_index.py wrote them, one zstd frame
    catalog.txt.zst    "HASH<tab>title<tab>which save<tab>contributors" for every icon
    icons/HASH.zst     icon.sys (964 bytes) then the icon, one zstd frame (every PS2IODB icon)
index.txt and catalog.txt are the same, uncompressed, to read by eye. Icon names are content hashes and never
change, so an update only adds icons and replaces the index.

One frame per file with its size in the header: the app decodes with the same JNI zstd decoder
the texture-pack installer uses.

Needs the zstandard module (pip install zstandard).
usage: publish_icons.py INDEX_DIR OUT_DIR    (INDEX_DIR holds index.txt and icons/ from build_index.py)
"""
import concurrent.futures, os, sys, zipfile
import zstandard

LEVEL = 19


def compress(path):
    data = open(path, "rb").read()
    packed = zstandard.ZstdCompressor(level=LEVEL, write_content_size=True).compress(data)
    assert zstandard.ZstdDecompressor().decompress(packed) == data, path
    return len(data), packed


def main():
    src, out = sys.argv[1:3]
    root = os.path.join(out, "memcard-icons")
    os.makedirs(os.path.join(root, "icons"), exist_ok=True)
    lines = [l for l in open(os.path.join(src, "index.txt")) if l.strip()]
    hashes = sorted(f[:-4] for f in os.listdir(os.path.join(src, "icons")) if f.endswith(".bin"))

    raw = packed = 0
    with concurrent.futures.ProcessPoolExecutor() as pool:
        paths = [os.path.join(src, "icons", h + ".bin") for h in hashes]
        for h, (n, data) in zip(hashes, pool.map(compress, paths, chunksize=32)):
            with open(os.path.join(root, "icons", h + ".zst"), "wb") as f:
                f.write(data)
            raw += n
            packed += len(data)

    for name in ("index.txt", "catalog.txt"):
        text = open(os.path.join(src, name), "rb").read()
        with open(os.path.join(root, name), "wb") as f:
            f.write(text)
        with open(os.path.join(root, name + ".zst"), "wb") as f:
            f.write(zstandard.ZstdCompressor(level=LEVEL, write_content_size=True).compress(text))

    # A plain zip written here, not by Finder: no __MACOSX or ._ entries.
    archive = os.path.join(out, "memcard-icons.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_STORED) as z:
        for name in ["index.txt", "index.txt.zst", "catalog.txt", "catalog.txt.zst"] + ["icons/%s.zst" % h for h in hashes]:
            z.write(os.path.join(root, name), "memcard-icons/" + name)
    print("%d icons: %.1f MB raw -> %.1f MB zstd -%d (%.0f%%); index %d serials, %.0f KB zstd; %s %.1f MB" % (
        len(hashes), raw / 1e6, packed / 1e6, LEVEL, 100 * packed / raw, sum(1 for l in lines if not l.startswith("#")),
        os.path.getsize(os.path.join(root, "index.txt.zst")) / 1e3, archive, os.path.getsize(archive) / 1e6))


if __name__ == "__main__":
    main()
