import unittest

from script import code_checks


class CodeChecksTests(unittest.TestCase):
    def test_anonymous_namespace_closure_does_not_create_a_named_namespace(self):
        source = "namespace {\n}\u0020// namespace\n\nnamespace {\n}\u0020// namespace\n"
        self.assertEqual(code_checks.check_namespace_comments({}, "example.cpp", source), [])

    def test_named_namespace_still_requires_matching_closing_comment(self):
        source = "namespace terrain::detail\n{\n}\u0020// namespace terrain\n"
        self.assertEqual(
            code_checks.check_namespace_comments({}, "example.cpp", source),
            ["missing closing comment for namespace terrain::detail"],
        )
