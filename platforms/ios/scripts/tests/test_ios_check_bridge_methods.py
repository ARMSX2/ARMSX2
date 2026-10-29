"""check_bridge_methods.py reads the selectors ARMSX2Bridge.h declares, and fails loudly where it cannot."""

import importlib.util
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO

from ios_source import IOS_ROOT

spec = importlib.util.spec_from_file_location("check_bridge_methods",
                                              IOS_ROOT / "scripts/check_bridge_methods.py")
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


def header(body):
    return "@interface ARMSX2Bridge : NSObject\n%s\n@end\n" % body


class CheckBridgeMethods(unittest.TestCase):
    def test_selectors(self):
        cases = {
            "- (void)setValue:(int)v\n         forKey:(NSString *)k;": "-setValue:forKey:",
            "+ (NSString *)biosName NS_SWIFT_NAME(biosName());": "+biosName",
            "- (void)load:(NSString *)p NS_SWIFT_NAME(load(path:));": "-load:",
            "- (void)fetch:(void (^)(BOOL ok, NSString *msg))done;": "-fetch:",
            "    + (BOOL)isRunning;": "+isRunning",
        }
        for decl, sel in cases.items():
            self.assertEqual(check.declared(header(decl)), {sel}, decl)

    def test_a_block_comment_opener_in_a_line_comment(self):
        text = header("// see Bridge/*.mm\n+ (void)a;\n- (void)b:(int)x c:(int)y;\n/* x */")
        self.assertEqual(check.declared(text), {"+a", "-b:c:"})

    def test_an_indented_end_closes_only_its_own_block(self):
        text = ("@interface ARMSX2Bridge : NSObject\n+ (void)mine;\n  @end\n"
                "@interface Other : NSObject\n+ (void)notBridge;\n@end\n")
        self.assertEqual(check.declared(text), {"+mine"})

    def test_a_conditional_inside_the_interface_exits_2(self):
        with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False) as fh:
            fh.write(header("#if !TARGET_OS_SIMULATOR\n+ (void)deviceOnly;\n#endif"))
        self.addCleanup(os.unlink, fh.name)
        out = StringIO()
        with redirect_stdout(out):
            self.assertEqual(check.main([fh.name, "/nonexistent"]), 2)
        self.assertIn("conditional declarations", out.getvalue())

    def test_compare(self):
        missing, duplicated = check.compare({"+a", "-b:"}, [["+a", "+a"], []])
        self.assertEqual((missing, duplicated), (["-b:"], ["+a"]))


if __name__ == "__main__":
    unittest.main()
