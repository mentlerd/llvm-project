"""
Test lldb data formatter subsystem.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class StdFunctionTestCase(TestBase):
    SHARED_BUILD_TESTCASE = False

    @add_test_categories(["libc++"])
    def test_libcxx(self):
        self.build(dictionary={"USE_LIBCPP": 1})
        lldbutil.run_to_source_breakpoint(self, "break 1", lldb.SBFileSpec("main.cpp"))
        self.check("free")
        self.check("callable")
        self.check("lambda")
        self.check("member")

        self.check("freeConverting")
        self.check("callableConverting")
        self.check("lambdaConverting")
        self.check("memberConverting")

        self.check("lambdaComplex")

        self.check("callableOverloadedInt")
        self.check("callableOverloadedIntBox")
        self.check("memberVirtual")

        self.check("block")
        self.check("blockConverting")

        self.assertFalse(True)

    def check(self, name):
        self.runCmd(f"p {name}", trace=True)
