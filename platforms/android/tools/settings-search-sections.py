#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Print the CollapsibleSection chain for every settings row label.

Walks the Android settings screens (platforms/android/app/src/**/ui/), finds the shared row
widgets (ToggleRow / IntSliderRow / SegmentedRow / SegmentedGridRow) and the CollapsibleSection
blocks that wrap them, and prints one line per row call site:

    <label key or raw expr>  <-  outermost.section > inner.section   (file:line)

Rows outside every section show "(top level)". The `sections` column of
app/src/main/java/com/armsx2/ui/settingshub/SettingsSearchIndex.kt comes from this: a search
jump opens the listed sections so the destination row is composed before it is selected.

Call sites whose label or section title is computed (helper functions with parameters, dynamic
titles like GameDbSection's count suffix) print their raw expression and need hand mapping —
resolve them through the helper's call sites when updating the index.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "app" / "src"

WIDGETS = ("ToggleRow", "IntSliderRow", "SegmentedRow", "SegmentedGridRow")
SECTION = "CollapsibleSection"
STR_RE = re.compile(r'str\(\s*"([^"]+)"\s*\)')
LIT_RE = re.compile(r'^"([^"]*)"$')


def match_bracket(text, i, open_ch, close_ch):
    """text[i] is open_ch; return the index just past its matching close_ch."""
    depth = 0
    while i < len(text):
        c = text[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
        elif c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        i += 1
    raise ValueError(f"unbalanced {open_ch}{close_ch}")


def parse_call(text, start):
    """Parse `Name(ARGS) [trailing lambda]` from a name at `start`.

    Returns (args_text, (content_start, content_end) | None) for the trailing lambda body.
    """
    i = text.index("(", start)
    args_end = match_bracket(text, i, "(", ")")
    args_text = text[i + 1:args_end - 1]
    j = args_end
    while j < len(text) and text[j] in " \t\n":
        j += 1
    lam = None
    if j < len(text) and text[j] == "{" and not text[args_end:j].strip():
        body_end = match_bracket(text, j, "{", "}")
        lam = (j + 1, body_end - 1)
    return args_text, lam


def first_arg(args_text):
    """First argument expression: `label = expr` counts, else the positional head."""
    m = re.search(r"\blabel\s*=\s*", args_text)
    expr = args_text[m.end():] if m else args_text
    depth = 0
    for i, c in enumerate(expr):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            expr = expr[:i]
            break
    return expr.strip()


def resolve(expr):
    """str("key") -> key, "literal" -> literal, anything else -> raw expression."""
    m = STR_RE.fullmatch(expr)
    if m:
        return m.group(1)
    m = LIT_RE.fullmatch(expr)
    if m:
        return m.group(1)
    return expr


def scan(path):
    text = path.read_text()
    rel = path.relative_to(ROOT)
    sections = []
    for m in re.finditer(r"\b" + SECTION + r"\s*\(", text):
        args, lam = parse_call(text, m.start())
        if lam is None:
            continue
        title = resolve(first_arg(args))
        sections.append((title, lam[0], lam[1]))

    for widget in WIDGETS:
        for m in re.finditer(r"\b" + widget + r"\s*\(", text):
            args, _ = parse_call(text, m.start())
            label = resolve(first_arg(args))
            chain = [s for s in sections if s[1] <= m.start() <= s[2]]
            line = text.count("\n", 0, m.start()) + 1
            where = " > ".join(s[0] for s in chain) or "(top level)"
            print(f"{label}\t<- {where}\t({rel}:{line})")


def main():
    for sub in sorted(ROOT.iterdir()):
        if not sub.is_dir() or sub.name in ("test", "androidTest"):
            continue
        for p in sorted(sub.rglob("*.kt")):
            if "/ui/" in str(p):
                scan(p)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        pass
