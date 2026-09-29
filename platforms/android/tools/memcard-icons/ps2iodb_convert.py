#!/usr/bin/env python3
"""Stream PS2IODB's GitHub tarball from stdin and convert every icon's normal pose back into the
PS2's own format: icon.sys (964 bytes, title blanked) followed by the icon file, the same .bin the
app already reads. Nothing of the tarball is written to disk except the converted icons.

PS2IODB export conventions (extractor/ps2iodbextractor/iconexport.py, website/src/extractor):
  OBJ v   = (-x, -y, z) / 4096 of shape 0, then r g b / 255, '#' a / 255
  OBJ vt  = u v / 4096, vn = normal / 4096, faces are sequential triangles
  PNG     = 128x128, texel row r at image row 127 - r, channels = 5-bit value << 3
  .anim   = frames of {shapeId, keys[{time,value}], vertexData = raw xyz / 4096 (not negated)}
  iconsys.json = raw icon.sys values: bg colours as hex of the stored 0..128 channels.

usage: curl -sL https://codeload.github.com/Issung/PS2IODB/tar.gz/refs/heads/main | python3 ps2iodb_convert.py OUT_DIR
"""
import hashlib, io, json, os, re, struct, sys, tarfile
from PIL import Image

OUT = sys.argv[1]
os.makedirs(os.path.join(OUT, "icons"), exist_ok=True)
records = open(os.path.join(OUT, "records.jsonl"), "w")
problems = open(os.path.join(OUT, "problems.txt"), "w")
ICON_PREFIX = "/website/public/icons/"
TITLES_SUFFIX = "/website/src/model/Titles.ts"


def s16(v):
    return max(-32768, min(32767, int(round(v))))


def u8(v):
    return max(0, min(255, int(round(v))))


def hexcol(s):
    s = (s or "#000000").lstrip("#")
    try:
        return [int(s[i:i + 2], 16) for i in (0, 2, 4)]
    except ValueError:
        return [0, 0, 0]


def num(v, default=0.0):
    try:
        return float(v) if v is not None else default
    except (TypeError, ValueError):
        return default


def floats4(v):
    v = list(v or []) + [0.0] * 4
    return [num(x) for x in v[:4]]


def iconsys_bytes(j, normal):
    b = bytearray(964)
    b[0:4] = b"PS2D"
    struct.pack_into("<I", b, 0x0C, max(0, min(255, int(num(j.get("bgOpacity"))))))
    for i, key in enumerate(("bgColTL", "bgColTR", "bgColBL", "bgColBR")):
        r, g, bl = hexcol(j.get(key))
        struct.pack_into("<IIII", b, 0x10 + i * 16, r, g, bl, 0)
    for i, key in enumerate(("light1Dir", "light2Dir", "light3Dir")):
        struct.pack_into("<4f", b, 0x50 + i * 16, *floats4(j.get(key)))
    for i, key in enumerate(("light1Col", "light2Col", "light3Col")):
        struct.pack_into("<4f", b, 0x80 + i * 16, *floats4(j.get(key)))
    struct.pack_into("<4f", b, 0xB0, *floats4(j.get("ambiLightCol")))
    name = normal.encode("ascii", "replace")[:63]
    for o in (0x104, 0x144, 0x184):
        b[o:o + len(name)] = name
    return bytes(b)


def parse_obj(text):
    vs, vts, vns, faces = [], [], [], []
    for line in text.splitlines():
        if not line or line[0] not in "vf":
            continue
        head, _, comment = line.partition("#")
        p = head.split()
        if not p:
            continue
        if p[0] == "v":
            a = float(comment.strip()) if comment.strip() else None
            vs.append(([float(x) for x in p[1:4]], [float(x) for x in p[4:7]] if len(p) >= 7 else None, a))
        elif p[0] == "vt":
            vts.append([float(x) for x in p[1:3]])
        elif p[0] == "vn":
            vns.append([float(x) for x in p[1:4]])
        elif p[0] == "f":
            corners = []
            for c in p[1:]:
                idx = (c.split("/") + ["", ""])[:3]
                corners.append(tuple(int(x) if x else 0 for x in idx))
            for k in range(1, len(corners) - 1):  # fan, though PS2IODB faces are triangles
                faces.append((corners[0], corners[k], corners[k + 1]))
    return vs, vts, vns, faces


def rle(tex):
    out, lit, i, n = [], [], 0, len(tex)

    def flush():
        nonlocal lit
        while lit:
            chunk, lit = lit[:0x7FFF], lit[0x7FFF:]
            out.append(0x10000 - len(chunk))
            out.extend(chunk)

    while i < n:
        j = i
        while j < n and tex[j] == tex[i] and j - i < 0x7FFF:
            j += 1
        if j - i >= 3:
            flush()
            out.append(j - i)
            out.append(tex[i])
        else:
            lit.extend(tex[i:j])
        i = j
    flush()
    return struct.pack("<%dH" % len(out), *out)


def texture_words(png_bytes):
    im = Image.open(io.BytesIO(png_bytes)).convert("RGB")
    if im.size != (128, 128):
        im = im.resize((128, 128), Image.NEAREST)
    px = im.load()
    tex = []
    for row in range(128):
        for x in range(128):
            r, g, b = px[x, 127 - row]
            tex.append((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10))
    return tex


def convert(code, files):
    def find(name):
        if name in files:
            return files[name]
        low = name.lower()
        return next((v for k, v in files.items() if k.lower() == low), None)

    raw_sys = find("iconsys.json")
    if raw_sys is None:
        return None, "no iconsys.json"
    j = json.loads(raw_sys.decode("utf-8", "replace"), strict=False)
    normal = j.get("normal") or ""
    obj = find(normal + ".obj")
    if obj is None:
        objs = [k for k in files if k.endswith(".obj")]
        if len(objs) != 1:
            return None, "no obj for normal icon %r (%d objs)" % (normal, len(objs))
        obj = files[objs[0]]
        normal = objs[0][:-4]
    text = obj.decode("utf-8", "replace")
    vs, vts, vns, faces = parse_obj(text)
    if not faces:
        return None, "obj has no faces"
    # Texture: the obj's mtllib -> that mtl's map_Kd (duplicates were merged into one png).
    png = None
    m = re.search(r"^mtllib (.+)$", text, re.M)
    mtl = find(m.group(1).strip()) if m else None
    if mtl:
        k = re.search(r"^map_Kd (.+)$", mtl.decode("utf-8", "replace"), re.M)
        png = find(k.group(1).strip()) if k else None
    png = png or find(normal + ".png")
    tex = texture_words(png) if png else [0x7FFF] * 16384

    corners = [c for f in faces for c in f]
    count = len(corners)
    positions0, normals, uvs, colors = [], [], [], []
    for vi, ti, ni in corners:
        (x, y, z), col, a = vs[vi - 1]
        positions0.append((s16(-x * 4096), s16(-y * 4096), s16(z * 4096)))
        u, v = vts[ti - 1] if 0 < ti <= len(vts) else (0.0, 0.0)
        uvs.append((s16(u * 4096), s16(v * 4096)))
        nx, ny, nz = vns[ni - 1] if 0 < ni <= len(vns) else (0.0, 0.0, 0.0)
        normals.append((s16(nx * 4096), s16(ny * 4096), s16(nz * 4096)))
        colors.append(tuple(u8(c * 255) for c in col) + (u8(a * 255) if a is not None else 128,) if col else (128, 128, 128, 128))
    sequential = all(c[0] == i + 1 for i, c in enumerate(corners))

    shapes = [positions0]
    frames = []
    frame_length, speed, offset = 1, 1.0, 0
    anim_version = None
    anim = find(normal + ".anim")
    if anim is not None:
        a = json.loads(anim.decode("utf-8", "replace"), strict=False)
        anim_version = a.get("version", 1)
        by_shape = {}
        raw_frames = a.get("frames") or []
        # Exports made before PS2IODB's shapeId fix label every frame shape 0 although each frame
        # carries its own shape's vertices; on PS2 icons frame i animates shape i, so use that.
        ids = [int(num(fr.get("shapeId"))) for fr in raw_frames]
        relabel = len(ids) > 1 and len(set(ids)) == 1
        for n, fr in enumerate(raw_frames):
            vd = fr.get("vertexData") or []
            sid = n if relabel else ids[n]
            if len(vd) == count * 3 and sequential and sid not in by_shape:
                by_shape[sid] = [(s16(vd[i * 3] * 4096), s16(vd[i * 3 + 1] * 4096), s16(vd[i * 3 + 2] * 4096)) for i in range(count)]
            keys = [(num(k.get("time")), num(k.get("value"))) for k in (fr.get("keys") or [])]
            # A frame with no keys gets one, as PS2IODB's own v3 export does.
            frames.append((sid, keys or [(0.0, 1.0 if not frames else 0.0)]))
        if by_shape:
            n_shapes = max(by_shape) + 1
            shapes = [by_shape.get(s, by_shape.get(0, positions0)) for s in range(n_shapes)]
            frame_length = int(num(a.get("frameLength"), 1))
            speed = num(a.get("animSpeed"), 1.0)
            offset = int(num(a.get("playOffset")))
        else:
            frames = []
    if not frames:
        frames = [(0, [(0.0, 1.0)])]

    out = bytearray()
    out += struct.pack("<IIIfI", 0x00010000, len(shapes), 0x0F, 1.0, count)
    for i in range(count):
        for shape in shapes:
            x, y, z = shape[i]
            out += struct.pack("<hhhh", x, y, z, 0)
        out += struct.pack("<hhhh", *normals[i], 0)
        out += struct.pack("<hh", *uvs[i])
        out += struct.pack("<BBBB", *colors[i])
    out += struct.pack("<IIfII", 1, frame_length, speed, offset, len(frames))
    for sid, keys in frames:
        out += struct.pack("<II", sid, len(keys))
        for t, v in keys:
            out += struct.pack("<ff", t, v)
    packed = rle(tex)
    out += struct.pack("<I", len(packed)) + packed

    data = iconsys_bytes(j, normal) + bytes(out)
    h = hashlib.sha1(data).hexdigest()[:16]
    with open(os.path.join(OUT, "icons", h + ".bin"), "wb") as f:
        f.write(data)
    return {
        "code": code, "hash": h, "directory": j.get("directory"), "normal": normal,
        "verts": count, "shapes": len(shapes), "frames": len(frames), "anim": anim_version, "relabelled": bool(anim is not None and relabel),
        "sequential": sequential, "texture": png is not None, "bytes": len(data),
    }, None


done = failed = 0
current, files = None, {}


def finish():
    global done, failed
    if current is None:
        return
    try:
        rec, why = convert(current, files)
    except Exception as e:  # one bad icon must not stop the rest
        rec, why = None, "%s: %s" % (type(e).__name__, e)
    if rec:
        records.write(json.dumps(rec) + "\n")
        done += 1
    else:
        problems.write("%s\t%s\n" % (current, why))
        failed += 1
    if (done + failed) % 250 == 0:
        print("converted %d, failed %d" % (done, failed), flush=True)


with tarfile.open(fileobj=sys.stdin.buffer, mode="r|gz") as tar:
    for m in tar:
        if not m.isfile():
            continue
        path = m.name
        if path.endswith(TITLES_SUFFIX):
            with open(os.path.join(OUT, "Titles.ts"), "wb") as f:
                f.write(tar.extractfile(m).read())
            continue
        i = path.find(ICON_PREFIX)
        if i < 0:
            continue
        rest = path[i + len(ICON_PREFIX):].split("/")
        if len(rest) != 2:
            continue
        code, name = rest
        if code != current:
            finish()
            current, files = code, {}
        if name.endswith(".mtl") or name.endswith(".png") or name.endswith(".obj") or name.endswith(".anim") or name == "iconsys.json":
            files[name] = tar.extractfile(m).read()
    finish()

records.close()
problems.close()
print("DONE converted %d, failed %d" % (done, failed), flush=True)
