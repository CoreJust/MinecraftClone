"""Regression coverage for shared source-check imports and annotations."""

import typing
import unittest

from script import checks_common


class ChecksCommonTests(unittest.TestCase):
    def test_source_spec_annotations_resolve(self):
        annotations = typing.get_type_hints(checks_common.SourceSpec)
        self.assertEqual(annotations["extensions"], typing.Set[str])


if __name__ == "__main__":
    unittest.main()
