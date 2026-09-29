#!/usr/bin/env python3
"""Index the icons ps2iodb_convert.py wrote by disc serial, for the app to fetch.

PS2IODB keys its icons by title, not serial. For each serial in GameIndex.yaml:
  1. an icon whose export recorded a save folder of that very serial ("directory"), pooled with
     the rest of that title's icons so a recorded System Data icon cannot beat the Game Data one
  2. otherwise an icon whose PS2IODB title matches the serial's GameIndex name
In both, the serial's own region's icon wins, then the main save's ("Save Data", "Game Data").

Writes OUT_DIR/index.txt ("SERIAL HASH" lines), OUT_DIR/icons/HASH.bin for every icon PS2IODB
has (the index names most; the rest are for the Icon Museum), OUT_DIR/catalog.txt ("HASH<tab>title
<tab>which save<tab>contributors" for every icon, for the Museum and its credits) and OUT_DIR/why.txt
with the reason for each serial, to review.

usage: build_index.py IODB_DIR GAMEINDEX OUT_DIR
"""
import json, os, re, shutil, sys, unicodedata, collections

IODB, GAMEINDEX, OUT = sys.argv[1:4]
os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)

FOLDER_SERIAL = re.compile(r"^B[A-Z]([A-Z]{4})[-_]?(\d{3})\.?(\d{2})")


def serial_of(folder):
    m = FOLDER_SERIAL.match((folder or "").upper())
    s = "%s-%s%s" % m.groups() if m else None
    return None if s is None or s.endswith("-00000") else s  # tools' and demos' placeholder serials


def norm(t):
    t = unicodedata.normalize("NFKD", t).encode("ascii", "ignore").decode().lower()
    t = t.replace("&", " and ").replace("'", "")
    t = re.sub(r"[\[(][^\])]*[\])]", " ", t)
    t = re.sub(r"[^a-z0-9 ]", " ", t)
    t = re.sub(r"\b(the|a|an)\b", " ", t)
    return re.sub(r"\s+", " ", t).strip()


def region(serial):
    p = serial[:4]
    if p in ("SLUS", "SCUS"): return "us"
    if p in ("SLES", "SCES", "SLED", "SCED"): return "eu"
    if p in ("SLPS", "SLPM", "SCPS", "SLKA", "SCKA", "SCAJ", "SLAJ", "PAPX", "PBPX"): return "jp"
    return ""


REGION_WORDS = {
    "us": ("north america", "americas", "usa", "(us)", "ntsc-u", "ntsc"),
    "eu": ("pal", "europe", "(eu)"),
    "jp": ("japan", "(jp)", "asia"),
}
MAIN_WORDS = ("game data", "save data", "story data", "progress", "career", "game record", "save", "worldwide")
AVOID_WORDS = ("system", "setting", "option", "replay", "ghost", "score", "roster", "edit", "profile",
               "netfile", "vtr", "photo", "emblem", "regulation", "record", "draft", "config", "cinema")


def label_rank(label, want):
    l = label.lower()
    score = 0
    if want and any(w in l for w in REGION_WORDS.get(want, ())): score += 100
    elif any(w in l for ws in REGION_WORDS.values() for w in ws): score -= 50  # another region's
    if any(w in l for w in MAIN_WORDS): score += 20
    if l.strip() in MAIN_WORDS: score += 15  # plain "Save Data" over "Dog Ending Save Data"
    if any(w in l for w in AVOID_WORDS): score -= 30
    return score


# ---- PS2IODB
records = [json.loads(l) for l in open(os.path.join(IODB, "records.jsonl"))]
by_code = {r["code"]: r for r in records}
src = open(os.path.join(IODB, "Titles.ts"), encoding="utf-8").read()
titles = collections.defaultdict(list)  # norm title -> [(label, code, order)]
codes_of_title = collections.defaultdict(list)  # PS2IODB title -> its icons
title_of_code = {}


def qstr(n):
    """A quoted string in Titles.ts, which mostly uses backticks but sometimes ' or "."""
    return r"""(?:`(?P<%sb>(?:[^`\\]|\\.)*)`|'(?P<%ss>(?:[^'\\]|\\.)*)'|"(?P<%sd>(?:[^"\\]|\\.)*)")""" % (n, n, n)


def qval(m, n):
    return next((v for v in (m.group(n + "b"), m.group(n + "s"), m.group(n + "d")) if v is not None), "")


GAME = r"new (?:Game|Application)\(\s*" + qstr("t")
ICON = r"new Icon\(\s*g\s*,\s*" + qstr("l") + r"\s*,\s*" + qstr("c")  # "new Icon(g , 'x', 'code'" occurs
for m in re.finditer(GAME + r"\s*,\s*(?:" + qstr("c") + r"|g\s*=>\s*\[(?P<block>.*?)\n\s*\]\))", src, re.S):
    title, code, block = qval(m, "t"), qval(m, "c") or None, m.group("block")
    entries = [("", code)] if code else [(qval(i, "l"), qval(i, "c")) for i in re.finditer(ICON, block or "")]
    for i, (label, c) in enumerate(entries):
        if c in by_code:
            for name in {title} | set(title.split(" / ")):  # "A / B" titles carry two names
                if norm(name):  # a kanji-only name normalises to nothing and must match nothing
                    titles[norm(name)].append((label, c, i))
            title_of_code[c] = (title, label)
            codes_of_title[title].append((label, c, i))

iodb_exact = collections.defaultdict(list)
for r in records:
    s = serial_of(r.get("directory"))
    if s: iodb_exact[s].append(r["code"])

# ---- GameIndex
gi = {}
cur = name = nameen = None
for raw in open(GAMEINDEX, encoding="utf-8"):
    raw = raw.rstrip("\n")
    if not raw or raw.startswith("#"): continue
    if not raw[0].isspace():
        if cur and (nameen or name): gi[cur] = (nameen, name)
        s = raw.split(":")[0].strip()
        cur = s.upper() if 8 <= len(s) <= 12 and s[4:5] == "-" else None
        name = nameen = None
    elif cur:
        t = raw.strip()
        val = lambda p: t[len(p):].strip().split(" #")[0].strip().strip('"')
        if t.startswith("name-en:"): nameen = val("name-en:")
        elif t.startswith("name:"): name = val("name:")
if cur and (nameen or name): gi[cur] = (nameen, name)

index, why = {}, {}
used_iodb = set()


def pick_iodb(serial, candidates, exact=()):
    want = region(serial)
    best = sorted(candidates, key=lambda e: (-(label_rank(e[0], want) + (10 if e[1] in exact else 0)), e[2]))
    return best[0][1]


serials = set(gi) | set(iodb_exact)
for s in sorted(x for x in serials if not x.endswith("-00000")):
    if s in iodb_exact:
        # The icons recorded from this serial's saves, pooled with the rest of the same title's
        # icons: a recorded System Data icon must not beat that game's Game Data one.
        codes = iodb_exact[s]
        pool = {}
        for x in codes:
            for e in codes_of_title.get(title_of_code.get(x, ("", ""))[0], []) or [("", x, 0)]:
                pool[e[1]] = e
            pool.setdefault(x, (title_of_code.get(x, ("", ""))[1], x, 0))
        c = pick_iodb(s, list(pool.values()), exact=set(codes))
        index[s] = by_code[c]["hash"]; why[s] = "iodb-serial " + c; used_iodb.add(c)
    else:
        c = None
        for nm in gi.get(s, (None, None)):
            if nm and norm(nm) in titles:
                c = pick_iodb(s, titles[norm(nm)]); break
        if c:
            index[s] = by_code[c]["hash"]; why[s] = "iodb-title " + c; used_iodb.add(c)

# Every icon's title, which save it is (Game Data, Japan...) and who contributed it, from the
# lines of Titles.ts: single-icon titles on one line, multi-icon titles as a Game line then Icons.
catalog = {}
game = None
for line in src.splitlines():
    g = re.search(GAME, line)
    if g:
        game = qval(g, "t")
        c = re.search(GAME + r"\s*,\s*" + qstr("c"), line)
        if c: catalog[qval(c, "c")] = (game, "", re.findall(r"Contributors\.(\w+)", line))
        continue
    i = re.search(ICON, line)
    if i and game: catalog[qval(i, "c")] = (game, qval(i, "l"), re.findall(r"Contributors\.(\w+)", line))
with open(os.path.join(OUT, "catalog.txt"), "w", encoding="utf-8") as f:
    for r in sorted(records, key=lambda r: r["code"]):
        title, label, who = catalog.get(r["code"], (r["code"], "", []))
        f.write("%s\t%s\t%s\t%s\n" % (r["hash"], title.replace("\t", " "), label.replace("\t", " "), ", ".join(who)))

# copy every icon: the index's, and the rest for the Museum
hashes = {r["hash"] for r in records}
for h in hashes:
    p = os.path.join(IODB, "icons", h + ".bin")
    if os.path.exists(p):
        shutil.copyfile(p, os.path.join(OUT, "icons", h + ".bin"))
    else:
        print("MISSING icon file", h)

with open(os.path.join(OUT, "index.txt"), "w") as f:
    f.write("# ARMSX2 memory card icons v1: SERIAL HASH, the icon is icons/HASH.bin (icon.sys, 964 bytes, then the icon)\n")
    for s in sorted(index): f.write("%s %s\n" % (s, index[s]))
with open(os.path.join(OUT, "why.txt"), "w") as f:
    for s in sorted(index): f.write("%s %s %s | %s\n" % (s, index[s], why[s], (gi.get(s, (None, None))[0] or gi.get(s, (None, None))[1] or "")))

kinds = collections.Counter(w.split()[0] for w in why.values())
size = sum(os.path.getsize(os.path.join(OUT, "icons", h + ".bin")) for h in hashes if os.path.exists(os.path.join(OUT, "icons", h + ".bin")))
print("serials: %d  %s  icons: %d in the index, %d in all (%.1f MB)  catalog: %d, %d without a title" % (
    len(index), dict(kinds), len(set(index.values())), len(hashes), size / 1e6, len(records), sum(1 for r in records if r["code"] not in catalog)))
us = [s for s in gi if s[:4] in ("SLUS", "SCUS")]
print("US serials covered: %d of %d; all GameIndex serials: %d of %d" % (
    sum(1 for s in us if s in index), len(us), sum(1 for s in gi if s in index), len(gi)))
