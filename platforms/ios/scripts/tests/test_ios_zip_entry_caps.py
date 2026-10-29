"""A zip entry is bounded by what came out of it, not by what it promised.

The extractors capped an entry on stat.size, which libzip only fills in when the
archive tells it the uncompressed size. An archive can withhold that, and then the
cap was skipped and ReadBinaryFileInZip grew a vector in 4 KiB steps until the
entry ended. The shader pack extractor was the only one checking the bytes it
actually got. Every read does now, so a new call site that only pre-checks
stat.size fails here instead of on someone's phone.
"""

import re
import unittest

from ios_source import BRIDGE_DIR, read


READ = "ReadBinaryFileInZip("
# The guard has to compare the bytes in hand against a named bound. A bare literal
# passes the compiler and tells the next reader nothing about which budget it is.
BOUND = re.compile(r"data->size\(\)\s*>\s*([A-Za-z_]\w*)")
# Every site sits within this many lines of its guard; more than that and the read
# and its check have drifted apart far enough to be worth looking at.
GUARD_LINES = 2


class ZipEntryCaps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lines, cls.where = [], []
        for path in sorted(BRIDGE_DIR.glob("ARMSX2Bridge*.mm")):
            # Block comments keep their newlines so the reported lines stay true.
            text = re.sub(r"//[^\n]*|/\*.*?\*/", lambda m: "\n" * m.group().count("\n"),
                          read(path), flags=re.S)
            for n, line in enumerate(text.split("\n"), 1):
                cls.lines.append(line)
                cls.where.append("%s:%d" % (path.name, n))
        cls.sites = [i for i, line in enumerate(cls.lines) if READ in line]

    def guard_at(self, index):
        return "\n".join(self.lines[index + 1:index + 1 + GUARD_LINES])

    def test_the_read_sites_were_actually_found(self):
        """Every check below passes on an empty list, so count the sites first."""
        self.assertGreaterEqual(
            len(self.sites), 6,
            "found %d %s call sites in the bridge files, expected at least 6. Either the "
            "reads moved behind a helper, in which case check the cap there, or this "
            "test is now looking at nothing" % (len(self.sites), READ))

    def test_every_read_is_bounded_by_what_it_got(self):
        unbounded = [
            "  " + self.where[i]
            for i in self.sites
            if not BOUND.search(self.guard_at(i))
        ]
        self.assertEqual(
            unbounded, [],
            "these zip reads are not capped on the bytes they returned, so an archive "
            "that withholds its uncompressed size allocates without limit:\n"
            + "\n".join(unbounded))

    def test_the_bounds_are_declared_and_not_literals(self):
        source = "\n".join(self.lines)
        found = (BOUND.search(self.guard_at(i)) for i in self.sites)
        # Unbounded sites are the other check's to report, not this one's.
        for name in sorted({m.group(1) for m in found if m}):
            with self.subTest(bound=name):
                self.assertRegex(
                    source, r"const zip_uint64_t\s+" + name + r"\s*=",
                    "%s bounds a zip read but is not declared as a zip_uint64_t in "
                    "the bridge files, so it is either a literal or the wrong width" % name)


if __name__ == "__main__":
    unittest.main()
