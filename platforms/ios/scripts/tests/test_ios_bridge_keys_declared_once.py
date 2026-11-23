"""The names and keys Swift and ObjC both use are declared once.

ARMSX2BridgeKeys.h declares them and ARMSX2BridgeKeys.mm holds the strings.
The ObjC compiler does not check which string a typed NSString key holds, and
Swift still accepts a literal outside a typed position, so a copied literal
would build and drift from the declaration without anyone noticing. These
tests fail on any such copy.
"""

import re
import unittest
from collections import Counter

from ios_source import BRIDGE_DIR, RUNTIME_DIR, SWIFT, read, without_comments


KEYS_H = BRIDGE_DIR / "ARMSX2BridgeKeys.h"
KEYS_MM = BRIDGE_DIR / "ARMSX2BridgeKeys.mm"

DECL = re.compile(r'(NSNotificationName|NSString \*|ARMSX2\w+) ?const (\w+) = @"([^"]+)";')
EXTERN = re.compile(r'extern (?:NSNotificationName|NSString \*|ARMSX2\w+) ?const (\w+);')
BACK_DOOR = re.compile(r'ARMSX2(?:PerGameKey|RAStateKey|GameLibraryKey|UISound)\(\s*rawValue:\s*"')

# (file name, value): reason. A file is never exempt as a whole.
OBJC_ALLOWED = {}


def declarations():
    """(type, constant name, value) for every definition in ARMSX2BridgeKeys.mm."""
    return [(kind.replace(" *", ""), name, value) for kind, name, value in DECL.findall(read(KEYS_MM))]


def objc_files():
    files = [p for d in (BRIDGE_DIR, RUNTIME_DIR) for ext in ("*.mm", "*.m", "*.h", "*.cpp")
             for p in sorted(d.glob(ext))]
    return [p for p in files if p not in (KEYS_H, KEYS_MM)]


def swift_files():
    return sorted(SWIFT.rglob("*.swift"))


def distinctive_values():
    """Values no unrelated Swift string is likely to equal.

    A single lowercase word such as serial is left to the compiler: in a key
    position only the typed constant compiles, and elsewhere it is some other
    string."""
    return {value for kind, _, value in declarations()
            if kind in ("NSNotificationName", "NSString") or not re.fullmatch(r"[a-z]+", value)}


class BridgeKeysDeclaredOnce(unittest.TestCase):
    def test_the_declarations_were_actually_found(self):
        # Every check below passes on an empty set, so count the declarations first.
        self.assertGreaterEqual(len(declarations()), 150, "parsed too few declarations from %s" % KEYS_MM.name)

    def test_header_and_definitions_match(self):
        externs = Counter(EXTERN.findall(read(KEYS_H)))
        defined = Counter(name for _, name, _ in declarations())
        self.assertEqual(sorted(n for n, c in externs.items() if c > 1), [], "declared twice in the header")
        self.assertEqual(sorted(n for n, c in defined.items() if c > 1), [], "defined twice in the .mm")
        self.assertEqual(sorted(set(externs) - set(defined)), [], "extern with no definition")
        self.assertEqual(sorted(set(defined) - set(externs)), [], "definition with no extern")

    def test_no_value_is_declared_twice_in_a_type(self):
        seen = Counter((kind, value) for kind, _, value in declarations())
        self.assertEqual(sorted(k for k, c in seen.items() if c > 1), [], "one value, two constants")

    def test_objc_spells_no_declared_value(self):
        values = {value for _, _, value in declarations()}
        found = []
        for path in objc_files():
            for value in set(re.findall(r'@"([^"]*)"', without_comments(read(path)))) & values:
                if (path.name, value) not in OBJC_ALLOWED:
                    found.append("%s: @\"%s\"" % (path.name, value))
        self.assertEqual(sorted(found), [], "use the ARMSX2BridgeKeys.h constant instead")

    def test_swift_spells_no_distinctive_value(self):
        values = distinctive_values()
        found = []
        for path in swift_files():
            for value in set(re.findall(r'"([^"\n]*)"', without_comments(read(path)))) & values:
                found.append("%s: \"%s\"" % (path.relative_to(SWIFT), value))
        self.assertEqual(sorted(found), [], "use the imported constant instead")

    def test_no_typed_key_from_a_string_literal(self):
        found = ["%s: %s" % (path.relative_to(SWIFT), m.group(0))
                 for path in swift_files() for m in BACK_DOOR.finditer(without_comments(read(path)))]
        self.assertEqual(found, [], "a typed key made from a literal skips the declaration")


if __name__ == "__main__":
    unittest.main()
