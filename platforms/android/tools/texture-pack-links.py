#!/usr/bin/env python3
"""
Builds the texture pack links the app shows in a pack's window (source, tip, socials, texture type,
status) from Sad Origami's Texture Packs Archive sheet, matched to the online catalog's packs.

usage: texture-pack-links.py <archive.xlsx> <out.json> <catalog.json> [--avatars] [--review <review.txt>]

  e.g. texture-pack-links.py "Texture Packs Archive.xlsx" texture-pack-links.json \
           textures.json --avatars --review review.txt

The file is not in the app: it is uploaded next to the catalog, as
https://dl.ps2ktxpak.net/texture-pack-links.json, and the app downloads it (TexturePackLinks.kt), so a
new upload changes every player's links without a release. Its "schemaVersion" is the format the app
reads; change it only for a change older apps must not read (they keep their last copy).

--avatars looks up each creator's profile picture online (GBAtemp, YouTube, GitHub) and checks it
loads; without it the file has none, so the app shows each creator's initials instead.

The xlsx is the sheet downloaded as Excel; the catalog is the textures.json the app reads,
https://dl.ps2ktxpak.net/textures.json. Standard library only. A pack is linked only when the sheet
says which of its listings the pack is; one it cannot place keeps the catalog's own source and
nothing else. --review writes every pack's outcome, for reading before the file goes in.
"""
import difflib
import json
import re
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import xml.etree.ElementTree as ET

NS = {
    "m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
    "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
    "pr": "http://schemas.openxmlformats.org/package/2006/relationships",
}
RID = "{%s}id" % NS["r"]

# The PS2 tab's columns.
TITLE, DOWNLOAD, REGION, SIZE, RESTRICTION, STATUS, TYPE, AUTHOR, DONATE, SOCIALS = 1, 2, 4, 5, 6, 7, 8, 9, 10, 11

# Titles the sheet spells differently from the catalog, checked by hand: catalog key -> sheet key.
TITLE_ALIASES = {
    "breathoffiredragonquarter": "breathoffirevdragonquarter",
    "007nightfire": "jamesbond007nightfire",
    "disneyspkoutofshadows": "pkoutofshadows",
    "disneystarzanuntamed": "tarzanuntamed",
    "jakii": "jak2renegade",
    "jakxcombatracing": "jakx",
    "magnacartatearsofblood": "magnacarta",
    "mastersofuniverseheman" "defenderofgrayskull": "hemandefenderofgrayskull",
    "silenthill4room": "silenthillroom",
}

# Where a game has several listings and the catalog names no creator, the one whose size matches the
# archive the pack came from (archive.org's pcsx2-hd-texture-packs) within a few percent:
# catalog key -> sheet author.
SIZE_PICKS = {
    "jak2renegade": "Curse_Arms",       # 508 MB archive; Curse_Arms 520 MB, the other is a 1.6 MB HUD
    "dothackguvol1rebirth": "mvp899",   # 1091 MB archive; mvp899 1.11 GB
}

# Packs the catalog credits to the wrong creator: pack id -> creator.
CREATOR_FIXES = {
    # The archive.org file is 486 MB; ironhulk33's own "Simpsons Road Rage 100% PCSX2 HD Pack.rar"
    # in his MediaFire folder is 487 MB. The sheet lists his for this game too.
    "texture-pack-credited-to-napoleongaming-by-the-curated-pcsx2-hd-texture-project-list-original-"
    "archive-filename-the-simpsons-road-rage-hd-remaster-community-archive-slus-20305-20260728": "ironhulk33",
}

# Other names a creator goes by, where their links say they are the same person: name -> sheet author.
NAME_ALIASES = {
    "IceBullet": "Curse_Arms",  # Curse_Arms's Ko-fi and YouTube are both "icebullet"
}

# A creator's links where the sheet's lead nowhere, checked by hand: name -> {field: url}.
CREATOR_LINKS = {
    # The sheet's @ironhulk33 YouTube link is dead; this is his channel (the user, 2026-10-02).
    "ironhulk33": {"socials": "https://www.youtube.com/channel/UC_00zPyx8PXENf7OSqZRw_g"},
    # His Ko-fi page is gone; he takes support on Patreon.
    "Bl4ckH4nd": {"tip": "https://www.patreon.com/Bl4ckH4nd"},
}

# The page a creator asked us to send people to, over the sheet's per-pack links.
CREATOR_PAGES = {
    # ironhulk33 (2026-10-02): his public MediaFire folder, PS2 subfolder.
    "ironhulk33": "https://www.mediafire.com/folder/k8yncbpnlckw3/ironhulk33#2ie0qajlodjxz",
}


# ---- the sheet ---------------------------------------------------------------------------------

def read_sheet(xlsx, name):
    z = zipfile.ZipFile(xlsx)

    def text_of(el):
        return "".join(t.text or "" for t in el.iter("{%s}t" % NS["m"]))

    def rels(path):
        try:
            root = ET.fromstring(z.read(path))
        except KeyError:
            return {}
        return {r.get("Id"): r.get("Target") for r in root.findall("pr:Relationship", NS)}

    shared = [text_of(si) for si in ET.fromstring(z.read("xl/sharedStrings.xml")).findall("m:si", NS)]
    wb = ET.fromstring(z.read("xl/workbook.xml"))
    targets = rels("xl/_rels/workbook.xml.rels")
    sheet = next(s for s in wb.find("m:sheets", NS) if s.get("name") == name)
    path = "xl/" + targets[sheet.get(RID)].lstrip("/").removeprefix("xl/")
    root = ET.fromstring(z.read(path))
    links = {}
    hl = root.find("m:hyperlinks", NS)
    if hl is not None:
        srels = rels(path.replace("worksheets/", "worksheets/_rels/") + ".rels")
        for h in hl.findall("m:hyperlink", NS):
            if h.get(RID) in srels:
                links[h.get("ref").split(":")[0]] = srels[h.get(RID)]

    def col(ref):
        n = 0
        for ch in re.match(r"[A-Z]+", ref).group(0):
            n = n * 26 + ord(ch) - 64
        return n - 1

    rows = []
    for row in root.find("m:sheetData", NS).findall("m:row", NS):
        cells = {}
        for c in row.findall("m:c", NS):
            v = c.find("m:v", NS)
            if c.get("t") == "s" and v is not None:
                val = shared[int(v.text)]
            elif c.get("t") == "inlineStr":
                val = text_of(c)
            else:
                val = v.text if v is not None else ""
            cells[col(c.get("r"))] = (val or "").strip(), links.get(c.get("r"))
        rows.append((int(row.get("r")), cells))
    return rows


def https(url):
    """The link as https, or None. The app opens nothing else, and the sheet's few http links are
    to sites that serve https too."""
    if not url:
        return None
    url = url.strip()
    if url.lower().startswith("http://"):
        url = "https://" + url[7:]
    return url if url.lower().startswith("https://") else None


def norm_status(s):
    return {"completed": "Complete", "complete": "Complete"}.get(s.lower(), s)


def norm_type(s):
    return "AI Upscale" if s.lower() == "ai upscale" else s


def listings(xlsx):
    out = []
    for n, cells in read_sheet(xlsx, "PS2"):
        get = lambda k: cells.get(k, ("", None))
        title = get(TITLE)[0]
        if n < 3 or not title:
            continue
        out.append(dict(
            row=n, title=title, download=https(get(DOWNLOAD)[1]), region=get(REGION)[0],
            size=get(SIZE)[0], status=norm_status(get(STATUS)[0]), type=norm_type(get(TYPE)[0]),
            author=get(AUTHOR)[0], author_url=https(get(AUTHOR)[1]),
            donate=https(get(DONATE)[1]), socials=https(get(SOCIALS)[1]),
        ))
    return out


# ---- names and titles --------------------------------------------------------------------------

def name_key(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def title_key(t):
    t = unicodedata.normalize("NFKD", t).encode("ascii", "ignore").decode().lower()
    t = t.replace("&", " and ")
    t = re.sub(r"\bthe\b", " ", t)
    return re.sub(r"[^a-z0-9]", "", t)


ROMAN = {"i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", "xi", "xii", "xiii"}


def numbers(t):
    """The numbers in a title, arabic or roman: two titles that differ only there are different games
    (Final Fantasy XII and X-2, Tony Hawk 3 and 4), however alike the rest reads."""
    words = re.findall(r"[a-z]+|\d+", t.lower())
    return {w for w in words if w.isdigit() or w in ROMAN}


def thread_id(url):
    p = urllib.parse.urlparse(url or "")
    if "gbatemp.net" not in p.netloc:
        return None
    m = re.search(r"/threads/(?:[^/]*\.)?(\d+)", p.path)
    return m.group(1) if m else None


def regions(serials):
    out = set()
    for s in serials:
        p = s[:4]
        if p in ("SLUS", "SCUS", "SLUD", "SCUD"):
            out.add("NTSC-U")
        elif p in ("SLES", "SCES", "SCED", "SLED"):
            out.add("PAL")
        elif p in ("SLPS", "SLPM", "SCPS", "SCAJ", "SLAJ", "SCPM", "SLPN"):
            out.add("NTSC-J")
        elif p in ("SLKA", "SCKA"):
            out.add("NTSC-K")
    return out


def region_fits(listing, serials):
    r = listing["region"]
    have = regions(serials)
    if r in ("", "N/A", "Multi", "All", "ISO"):
        return True
    if r == "NTSC/PAL":
        return bool(have & {"NTSC-U", "PAL"})
    return r in have


def catalog_creators(e):
    """The creator names a catalog entry gives. Its authors field is sometimes a sentence split at
    'and' ("HD texture pack created", "maintained by JustShrub."); the name is pulled out of those,
    and an entry that says it does not know its creator gives none."""
    text = " and ".join(e.get("authors", []))
    if re.search(r"not identified|Community Archive$|community mirror$", text, re.I):
        return []
    m = re.search(r"credited to (.+?) by the curated", text)
    if m:
        return [a.strip() for a in m.group(1).split("|")]
    m = re.search(r"(?:created|maintained) by (.+?)(?: \(| for |\.?$)", text)
    if m:
        return [m.group(1).strip()]
    if re.search(r"texture|pack\b|created|maintained", text, re.I):
        return []
    m = re.match(r"(\S+) \(repository mirror by", text)
    if m:
        return [m.group(1)]
    # "KevinMI Upscales: SomberShroud" is two people.
    return [p.strip() for a in e.get("authors", []) for p in re.split(r"[:|]", a) if p.strip()]


# ---- matching ----------------------------------------------------------------------------------

class Sheet:
    def __init__(self, rows):
        self.rows = rows
        self.by_thread, self.by_title = {}, {}
        for r in rows:
            t = thread_id(r["download"])
            if t:
                self.by_thread.setdefault(t, []).append(r)
            self.by_title.setdefault(title_key(r["title"]), []).append(r)
        # Each creator's names (as listed, and the gbatemp account names in their profile links, which
        # keep a creator's old name after a rename) and their usual links.
        self.names, self.links = {}, {}
        for r in rows:
            a = r["author"]
            if not a or a == "N/A":
                continue
            keys = self.names.setdefault(a, set())
            for part in re.split(r"\s*[|/,&]\s*|\s+and\s+", a):
                if part:
                    keys.add(name_key(part))
            m = re.search(r"/members/([^./]+)\.\d+", r["author_url"] or "")
            if m:
                keys.add(name_key(m.group(1)))
            for f in ("donate", "socials", "author_url"):
                if r[f]:
                    self.links.setdefault((a, f), {}).setdefault(r[f], 0)
                    self.links[(a, f)][r[f]] += 1

        for alias, author in NAME_ALIASES.items():
            self.names.setdefault(author, set()).add(name_key(alias))

    def authors_for(self, names):
        """Every sheet author a catalog creator name belongs to: listed alone, or with a partner
        ("Moataz | paynexkiller"). Alone first, so their own links win over a pair's."""
        alone, paired = [], []
        for n in names:
            k = name_key(n)
            for a, keys in self.names.items():
                if k in keys:
                    (paired if re.search(r"[|/,&]|\s+and\s+", a) else alone).append(a)
        return list(dict.fromkeys(alone + paired))

    def usual(self, author, field):
        counts = self.links.get((author, field))
        return max(counts, key=counts.get) if counts else None

    def hub(self, author):
        """The one page a creator lists (nearly) all their packs on, like a Patreon, or None."""
        downloads = [r["download"] for r in self.rows if r["author"] == author and r["download"]]
        if len(downloads) < 3:
            return None
        top = max(set(downloads), key=downloads.count)
        return top if downloads.count(top) >= 0.8 * len(downloads) else None

    def candidates(self, e):
        tid = thread_id(e.get("sourceUrl"))
        if tid and tid in self.by_thread:
            return self.by_thread[tid], "thread"
        k = title_key(e["gameTitle"] or e["name"])
        k = TITLE_ALIASES.get(k, k)
        if k in self.by_title:
            return self.by_title[k], "title"
        want = numbers(e["gameTitle"] or e["name"])
        for close in difflib.get_close_matches(k, list(self.by_title), n=5, cutoff=0.86):
            pool = [r for r in self.by_title[close] if numbers(r["title"]) == want]
            if pool:
                return pool, "close title"
        return [], ""


# Packs the catalog names no creator for, whose one listing in the sheet is a very different size from
# the archive the catalog took them from (archive.org's pcsx2-hd-texture-packs): likely another pack.
UNCONFIRMED = {
    "original-texture-creator-is-not-identified-in-the-public-mirror-preserved-by-the-pcsx2-hd-texture-"
    "packs-community-archive-crash-bandicoot-wrath-of-cortex-hd-remaster-community-archive-slus-20238-20260728",
    "original-texture-creator-is-not-identified-in-the-public-mirror-preserved-by-the-pcsx2-hd-texture-"
    "packs-community-archive-yu-gi-oh-duelist-of-the-roses-hd-remaster-community-archive-slus-20515-20260728",
}

# Hosts that only mirror packs: a creator's own page beats them as the source.
MIRRORS = ("archive.org", "mediafire.com", "4pda.to")


def same_link(a, b):
    if not a or not b:
        return False
    ta, tb = thread_id(a), thread_id(b)
    if ta or tb:
        return ta == tb
    norm = lambda u: urllib.parse.unquote(u).rstrip("/").lower()
    return norm(a) == norm(b)


def link(e, sheet):
    """(fields, how) for one catalog entry: link_listing's answer, sent to the page a creator asked
    for when they asked for one."""
    fields, how = link_listing(e, sheet)
    if fields and fields.get("creator"):
        for name, fixes in CREATOR_LINKS.items():
            if name_key(name) == name_key(fields["creator"].split(", ")[0]):
                fields.update(fixes)
        for name, page in CREATOR_PAGES.items():
            if name_key(name) in {name_key(c) for c in fields["creator"].split(",")}:
                fields["source"] = page
                how += ", creator's own page"
    return fields, how


def link_listing(e, sheet):
    """(fields, how) for one catalog entry; fields is None when there is nothing to add.

    The pack's own listing gives everything. A creator the sheet knows, but not for this pack, still
    gets their tip and socials links (and their one page, for a pack the catalog only has a mirror
    of). A creator the sheet does not know at all still gets a clean name when the catalog's is a
    sentence."""
    names = [CREATOR_FIXES[e["id"]]] if e["id"] in CREATOR_FIXES else catalog_creators(e)
    authors = sheet.authors_for(names)
    cands, how = sheet.candidates(e)
    k = title_key(e["gameTitle"] or e["name"])
    k = TITLE_ALIASES.get(k, k)
    if how == "thread":
        # A thread can carry many games (a creator's "library" thread): narrow it to this one.
        same = [r for r in cands if title_key(r["title"]) == k]
        cands = same or cands

    def creator_only(why):
        if authors:
            a = authors[0]
            out = dict(creator=a.replace(" | ", ", "), tip=sheet.usual(a, "donate"),
                       socials=sheet.usual(a, "socials"))
            if any(m in urllib.parse.urlparse(e.get("sourceUrl", "")).netloc for m in MIRRORS):
                # Their one page for all their packs, else their GBAtemp profile: either beats a mirror.
                out["source"] = sheet.hub(a) or sheet.usual(a, "author_url")
            return out, why + ", creator's links only"
        if names and " ".join(e.get("authors", [])) != " ".join(names):
            return dict(creator=", ".join(names)), why + ", name cleaned"
        return None, why

    if names:
        # The creator first, then the region: a creator's listing for another region of the game is
        # still their page, and someone else's for this region never is.
        mine = [r for r in cands if r["author"] in authors]
        if not mine:
            return creator_only("creator not listed for this game" if cands else "not in the sheet")
        cands = mine
    elif not cands:
        return None, "not in the sheet"
    cands = [r for r in cands if region_fits(r, e["serials"])] or cands
    exact = [r for r in cands if same_link(r["download"], e.get("sourceUrl"))]
    if exact:
        cands = exact
    if not names:
        pick = [r for r in cands if r["author"] == SIZE_PICKS.get(k)]
        if pick:
            cands = pick
        if how != "thread" and len(cands) > 1:
            return None, "unknown creator, %d listings" % len(cands)
        if e["id"] in UNCONFIRMED:
            return None, "unknown creator, size disagrees"
    if len({r["download"] for r in cands}) > 1:
        # The same creator made more than one pack for this game and the catalog does not say which.
        return creator_only(how + ", %d of their packs for this game" % len(cands))
    r = cands[0]
    return dict(source=r["download"], creator=r["author"].replace(" | ", ", "),
                tip=r["donate"] or sheet.usual(r["author"], "donate"),
                socials=r["socials"] or sheet.usual(r["author"], "socials"),
                type=r["type"], status=r["status"]), how + (", unknown creator" if not names else "")


# ---- creators' pages and profile pictures -----------------------------------------------------

USER_AGENT = "ARMSX2-texture-pack-links (+https://github.com/ARMSX2/ARMSX2)"


def fetch(url, follow=True):
    """(status, headers, body) for a GET, or None when the request fails outright."""
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None

    opener = urllib.request.build_opener() if follow else urllib.request.build_opener(NoRedirect)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with opener.open(req, timeout=30) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, b""
    except Exception:
        return None


def is_image(url):
    r = fetch(url)
    return bool(r) and r[0] == 200 and (r[1].get("Content-Type") or "").startswith("image/")


def gbatemp_avatar(profile):
    """A GBAtemp member's profile picture (384 px), at XenForo's fixed path, if they set one."""
    m = re.search(r"gbatemp\.net/members/(?:[^/]*\.)?(\d+)/?", profile or "")
    if not m:
        return None
    member = int(m.group(1))
    url = "https://gbatemp.net/data/avatars/h/%d/%d.jpg" % (member // 1000, member)
    return url if is_image(url) else None


def youtube_avatar(socials):
    """A YouTube channel's picture, for a channel-id link (handle links do not resolve this way)."""
    if not re.search(r"youtube\.com/channel/UC[\w-]+", socials or ""):
        return None
    r = fetch(socials)
    m = r and r[0] == 200 and re.search(rb'<meta property="og:image" content="(https://yt3\.[^"]+)"', r[2])
    if not m:
        return None
    url = re.sub(r"=s\d+-.*$", "=s400-c-k-c0x00ffffff-no-rj", m.group(1).decode())
    return url if is_image(url) else None


def github_avatar(owner):
    if not owner:
        return None
    r = fetch("https://github.com/%s.png?size=400" % owner, follow=False)
    location = r and r[1].get("Location")
    return location if location and is_image(location) else None


def github_owner(name, sources):
    """The GitHub account behind one of a creator's sources, when it is theirs by name."""
    for s in sources:
        m = re.match(r"https://github\.com/([^/]+)/", s or "")
        if m and name_key(m.group(1)) == name_key(name):
            return m.group(1)
    return None


def creators(sheet, packs, entries, resolve):
    """Each creator's page and profile picture: name -> {"page", "avatar"}. The picture is the one on
    their GBAtemp profile, else their YouTube channel's, else their GitHub one; Patreon's are left out,
    because its image links expire within weeks. Pictures are looked up only with resolve."""
    sources = {}
    for pid, p in packs.items():
        name = p.get("creator", "").split(", ")[0]
        if name:
            sources.setdefault(name, []).extend([p.get("source"), entries[pid].get("sourceUrl")])
    out = {}
    for name in sorted(sources):
        profile = sheet.usual(name, "author_url")
        socials = CREATOR_LINKS.get(name, {}).get("socials") or sheet.usual(name, "socials")
        owner = github_owner(name, sources[name])
        page = profile or socials or (owner and "https://github.com/" + owner)
        rec = {"page": page} if page else {}
        if resolve:
            avatar = gbatemp_avatar(profile) or youtube_avatar(socials) or github_avatar(owner)
            if avatar:
                rec["avatar"] = avatar
            time.sleep(0.3)
        if rec:
            out[name] = rec
    return out


def main(argv):
    review_path = None
    if "--review" in argv:
        i = argv.index("--review")
        review_path = argv[i + 1]
        del argv[i:i + 2]
    resolve = "--avatars" in argv
    if resolve:
        argv.remove("--avatars")
    xlsx, out, catalogs = argv[1], argv[2], argv[3:]
    sheet = Sheet(listings(xlsx))
    entries = {}
    for path in catalogs:
        for e in json.load(open(path))["entries"]:
            entries.setdefault(e["id"], e)
    packs, review = {}, []
    for pid, e in sorted(entries.items()):
        fields, how = link(e, sheet)
        review.append((how, e, fields))
        if not fields:
            # A pack no one has named a creator for says so, rather than showing the catalog's
            # sentence about it as a name.
            if not catalog_creators(e):
                packs[pid] = {"unknown": True}
            continue
        # Only what the sheet adds: a field it has nothing for is left out, and the app falls back.
        packs[pid] = {k: v for k, v in fields.items() if v and v not in ("Unknown", "N/A", "TBD", "TBA")}
    people = creators(sheet, packs, entries, resolve)
    line = lambda k, v: "  %s: %s" % (json.dumps(k), json.dumps(v, ensure_ascii=False))
    with open(out, "w", encoding="utf-8") as f:
        f.write('{\n "schemaVersion": 1,\n "about": "Texture Packs Archive by Sad Origami (solo.to/sadorigami): each pack\'s source, '
                'its creator, their tip and socials links, texture type and status; each creator\'s page '
                'and profile picture.",\n "creators": {\n')
        f.write(",\n".join(line(k, v) for k, v in sorted(people.items())))
        f.write('\n },\n "packs": {\n')
        f.write(",\n".join(line(k, v) for k, v in sorted(packs.items())))
        f.write("\n }\n}\n")
    if resolve:
        print("creators: %d, with a profile picture: %d" % (len(people), sum(1 for v in people.values() if "avatar" in v)))
    if review_path:
        with open(review_path, "w") as f:
            for how, e, fields in sorted(review, key=lambda x: (x[0], x[1]["gameTitle"])):
                f.write("%-48s %s %s by %s | %s\n" % (how, e["gameTitle"], e["serials"],
                        catalog_creators(e) or "?", e["sourceUrl"]))
                if fields:
                    f.write("%-48s   -> %s\n" % ("", fields))
    linked = sum(1 for v in packs.values() if not v.get("unknown"))
    print("catalog packs: %d, linked: %d, left as they are: %d" % (len(entries), linked, len(entries) - linked))


if __name__ == "__main__":
    main(sys.argv)
