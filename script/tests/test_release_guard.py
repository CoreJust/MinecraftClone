from __future__ import annotations

import unittest

from script.ci.release_guard import ReleaseRefError, validate_push, version_key


SHA = "a" * 40
ZERO = "0" * 40
TAG = "refs/tags/ai/EarlyDev/0.1.0/4_26.09.13"
REVISION_TAG = "refs/tags/ai/EarlyDev/0.1.0/4-r1_26.09.14"
REVISION_2_TAG = "refs/tags/ai/EarlyDev/0.1.0/4-r2_26.09.15"


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
            "refs/tags/ai/EarlyDev/0.1.0/4-r0_26.09.13",
            "refs/tags/ai/EarlyDev/0.1.0/4-r01_26.09.13",
        ):
            with self.subTest(remote_ref=remote_ref), self.assertRaisesRegex(ReleaseRefError, "contract|canonical|pushed"):
                validate_push([f"{remote_ref} {SHA} {remote_ref} {ZERO}"], [])

    def test_accepts_one_immutable_revision_of_an_existing_snapshot(self):
        validate_push([f"{REVISION_TAG} {SHA} {REVISION_TAG} {ZERO}"], [TAG])
        with self.assertRaisesRegex(ReleaseRefError, "one canonical"):
            validate_push([f"{REVISION_TAG} {SHA} {REVISION_TAG} {ZERO}"], [TAG, REVISION_TAG])

    def test_rejects_revision_without_canonical_tag_or_with_a_gap(self):
        with self.assertRaisesRegex(ReleaseRefError, "requires the canonical"):
            validate_push([f"{REVISION_TAG} {SHA} {REVISION_TAG} {ZERO}"], [])
        with self.assertRaisesRegex(ReleaseRefError, "revision 1"):
            validate_push([f"{REVISION_2_TAG} {SHA} {REVISION_2_TAG} {ZERO}"], [TAG])
        validate_push([f"{REVISION_2_TAG} {SHA} {REVISION_2_TAG} {ZERO}"], [TAG, REVISION_TAG])
        with self.assertRaisesRegex(ReleaseRefError, "already exists"):
            validate_push([f"{TAG} {SHA} {TAG} {ZERO}"], [REVISION_TAG])

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
        self.assertEqual(version_key(REVISION_TAG), "EarlyDev/0.1.0/4-r1")
        self.assertIsNone(version_key("refs/tags/ai/EarlyDev/0.1.0/4_26.09.13-maintenance"))


if __name__ == "__main__":
    unittest.main()
