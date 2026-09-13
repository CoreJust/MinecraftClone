from __future__ import annotations

import unittest

from script.ci.release_guard import ReleaseRefError, validate_push, version_key


SHA = "a" * 40
ZERO = "0" * 40
TAG = "refs/tags/ai/EarlyDev/0.1.0/4_26.09.13"


class ReleaseGuardTests(unittest.TestCase):
    def test_accepts_only_canonical_branches_and_new_version_tag(self):
        validate_push(
            [f"refs/heads/ai-dev {SHA} refs/heads/ai-dev {ZERO}", f"{TAG} {SHA} {TAG} {ZERO}"],
            [],
        )

    def test_rejects_auxiliary_branch_and_maintenance_tag(self):
        for remote_ref in (
            "refs/heads/codex/ai-release",
            "refs/heads/dev",
            "refs/heads/main",
            "refs/tags/ai/EarlyDev/0.1.0/4_26.09.13-maintenance",
        ):
            with self.subTest(remote_ref=remote_ref), self.assertRaisesRegex(ReleaseRefError, "contract|canonical|pushed"):
                validate_push([f"{remote_ref} {SHA} {remote_ref} {ZERO}"], [])

    def test_rejects_duplicate_version_and_retag(self):
        with self.assertRaisesRegex(ReleaseRefError, "one canonical"):
            validate_push([f"{TAG} {SHA} {TAG} {ZERO}"], [TAG])
        with self.assertRaisesRegex(ReleaseRefError, "immutable"):
            validate_push([f"{TAG} {SHA} {TAG} {SHA}"], [])

    def test_rejects_tag_present_on_destination_even_when_local_is_stale(self):
        with self.assertRaisesRegex(ReleaseRefError, "one canonical"):
            validate_push([f"{TAG} {SHA} {TAG} {ZERO}"], [], [TAG])

    def test_version_key_excludes_date(self):
        self.assertEqual(version_key(TAG), "EarlyDev/0.1.0/4")
        self.assertIsNone(version_key("refs/tags/ai/EarlyDev/0.1.0/4_26.09.13-maintenance"))


if __name__ == "__main__":
    unittest.main()
