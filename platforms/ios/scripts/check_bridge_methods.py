#!/usr/bin/env python3
"""Fail the build when a method ARMSX2Bridge.h declares has no linked body.

    python3 check_bridge_methods.py <ARMSX2Bridge.h> [<binary>]

Swift reaches the bridge through the ObjC runtime, so a declared method that lost its
body still links and only fails when it is called. This compares the methods every
@interface ARMSX2Bridge block declares with the ones `xcrun dyld_info -objc` lists in the
binary and, when the binary links it, in <binary>.debug.dylib, where a Debug build keeps
the code. As an Xcode build phase the binary comes from TARGET_BUILD_DIR and
EXECUTABLE_PATH. Exits 1 on a missing or doubly implemented method and 2 when either side
reads nothing or the header puts a preprocessor conditional inside an ARMSX2Bridge block.
"""

import os
import re
import subprocess
import sys

METHOD = re.compile(r"([+-])\[ARMSX2Bridge(?:\(\w+\))? ([^\]]+)\]")


def declared(text):
    # One pass, so a /* inside a // comment does not open a block comment.
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    found = set()
    for body in re.findall(r"^@interface ARMSX2Bridge\b[^\n]*\n(.*?)^\s*@end\b", text, re.M | re.S):
        if re.search(r"^\s*#\s*(?:if|elif|else|endif)", body, re.M):
            raise ValueError("conditional declarations are not supported")
        for kind, decl in re.findall(r"^\s*([+-])\s*(\(.*?);", body, re.M | re.S):
            prev = None
            while prev != decl:
                prev, decl = decl, re.sub(r"\([^()]*\)", " ", decl)
            parts = re.findall(r"(\w+)\s*:", decl)
            found.add(kind + ("".join(p + ":" for p in parts) if parts else decl.split()[0]))
    return found


def implemented(image):
    out = subprocess.run(["xcrun", "dyld_info", "-objc", image],
                         capture_output=True, text=True, check=True).stdout
    # Each slice of a fat image lists the same methods under its own header line.
    slices = re.split(r"^\S.*\]:$", out, flags=re.M)
    first = next((s for s in slices if s.strip()), "")
    return [kind + sel for kind, sel in METHOD.findall(first)]


def links(image, dylib):
    out = subprocess.run(["xcrun", "otool", "-L", image],
                         capture_output=True, text=True, check=True).stdout
    return "/" + os.path.basename(dylib) + " " in out


def compare(declared, per_image):
    linked = set()
    duplicated = set()
    for methods in per_image:
        linked.update(methods)
        duplicated.update(m for m in set(methods) if methods.count(m) > 1)
    return sorted(declared - linked), sorted(duplicated)


def binary_path(argv):
    build_dir = os.environ.get("TARGET_BUILD_DIR")
    exe = os.environ.get("EXECUTABLE_PATH")
    path = os.path.join(build_dir, exe) if build_dir and exe else argv[1]
    bundled = os.path.join(os.path.dirname(path), "Contents", "MacOS", os.path.basename(path))
    if not os.path.exists(path) and os.path.exists(bundled):
        return bundled
    return path


def main(argv):
    from_xcode = os.environ.get("TARGET_BUILD_DIR") and os.environ.get("EXECUTABLE_PATH")
    if not argv or (len(argv) < 2 and not from_xcode):
        print(__doc__.strip())
        return 2
    header = argv[0]
    with open(header, encoding="utf-8") as fh:
        try:
            decl = declared(fh.read())
        except ValueError as e:
            print("error: %s: %s" % (header, e))
            return 2
    if not decl:
        print("error: read no ARMSX2Bridge methods from %s" % header)
        return 2

    binary = binary_path(argv)
    dylib = binary + ".debug.dylib"
    images = [binary]
    per_image = []
    try:
        # A Release build over a Debug one leaves the old dylib in the bundle, unlinked.
        if os.path.exists(dylib) and links(binary, dylib):
            images.append(dylib)
        for image in images:
            per_image.append(implemented(image))
    except (OSError, subprocess.CalledProcessError) as e:
        print("error: reading %s failed: %s" % (binary, e))
        return 2
    if not any(per_image):
        print("error: found no ARMSX2Bridge methods in %s" % " or ".join(images))
        return 2

    missing, duplicated = compare(decl, per_image)
    for sel in missing:
        print("error: ARMSX2Bridge.h declares %s but no bridge file implements it" % sel)
    for sel in duplicated:
        n = max(methods.count(sel) for methods in per_image)
        print("error: %s is implemented %d times" % (sel, n))
    if missing or duplicated:
        return 1
    print("bridge methods: %d declared, all linked" % len(decl))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
