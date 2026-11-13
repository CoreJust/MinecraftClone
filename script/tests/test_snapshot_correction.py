from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))
import snapshot_correction


TAG = "ai/EarlyDev/0.1.1/2-r1_26.11.13"
PRIOR = "ai/EarlyDev/0.1.1/2_26.11.12"
TASK = "MC-AI-0683"


class SnapshotCorrectionTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        self.root = directory / "repo"
        self.root.mkdir()
        self.evidence = directory / "evidence"
        self.evidence.mkdir()
        self.git("init", "-b", "ai-main", "-q")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.test")
        self.write("publish.py", "raise SystemExit(0)\n")
        self.write("src/shared/include/shared/ProjectInfo.hpp", """
MAJOR_VERSION_NAME{"EarlyDev"};
MINOR_VERSION_NAME{"BasicBlocks"};
.epoch = 0, .major = 1, .minor = 1, .patch = 2,
""")
        self.write("src/game.cpp", "original\n")
        self.commit("root")
        self.git("switch", "-c", "published-source")
        self.write("README.md", "# Minecraft\n")
        self.commit("published source")
        self.git("switch", "ai-main")
        self.git("merge", "--no-ff", "published-source", "-m", "published promotion")
        self.base = self.git("rev-parse", "HEAD")
        self.git("tag", "-a", PRIOR, "-m", "AI snapshot MC-AI-0483")
        self.git("switch", "-c", "correction", "published-source")
        self.write("src/game.cpp", "corrected\n")
        self.source = self.commit(f"repair\n\nTask-ID: {TASK}")
        self.git("switch", "ai-main")
        self.git("merge", "--no-ff", "correction", "-m", f"promotion\n\nTask-ID: {TASK}")
        self.promoted = self.git("rev-parse", "HEAD")
        self.tree = self.git("rev-parse", "HEAD^{tree}")
        self.receipt = {
            **self.identity,
            "platforms": {
                platform: {
                    "artifact": self.evidence_file(platform + suffix, "artifact"),
                    "tests": [self.evidence_file(platform + "-tests.log", "Ran 1 test in 0.001s\n\nOK\n")],
                    "runtime": self.evidence_file(platform + "-runtime.log", "runtime passed"),
                    "source_files": {"src/game.cpp": hashlib.sha256(b"corrected\n").hexdigest()},
                }
                for platform, suffix in snapshot_correction.PLATFORMS.items()
            },
        }
        self.receipt["review"] = self.review_file()
        self.receipt_path = self.evidence / "receipt.json"

    @property
    def identity(self) -> dict:
        return {"base": self.base, "source": self.source, "tree": self.tree}

    def git(self, *arguments: str) -> str:
        environment = {**os.environ, "GIT_AUTHOR_DATE": "2026-11-13T12:00:00Z",
            "GIT_COMMITTER_DATE": "2026-11-13T12:00:00Z"}
        return subprocess.check_output(
            ["git", *arguments], cwd=self.root, env=environment, text=True, stderr=subprocess.PIPE,
        ).strip()

    def write(self, filename: str, text: str) -> None:
        path = self.root / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def commit(self, message: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-qm", message)
        return self.git("rev-parse", "HEAD")

    def evidence_file(self, filename: str, text: str) -> dict:
        payload = text.encode("utf-8")
        (self.evidence / filename).write_bytes(payload)
        return {"path": filename, "sha256": hashlib.sha256(payload).hexdigest()}

    def review_file(self, **overrides: str) -> dict:
        review = {
            **self.identity, "verdict": "approved", "reviewer": "Independent reviewer", "evidence": "Checked delta",
            "platforms_sha256": snapshot_correction.platform_digest(self.receipt["platforms"]),
            **overrides,
        }
        return self.evidence_file("review.json", json.dumps(review))

    def verify(self, **options: str) -> str:
        self.receipt_path.write_text(json.dumps(self.receipt), encoding="utf-8")
        return snapshot_correction.verify(self.root, self.receipt_path, self.base, TAG, self.promoted, **options)

    def test_verifies_exact_branch_push_and_annotated_tag_push(self) -> None:
        self.assertEqual(self.verify(push_ref="refs/heads/ai-main", remote_sha=self.base), TASK)
        self.git("tag", "-a", TAG, "-m", f"AI snapshot {TASK}")
        self.assertEqual(self.verify(push_ref=f"refs/tags/{TAG}", remote_sha="0" * 40), TASK)

    def test_rejects_remote_branch_drift(self) -> None:
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "exact checked remote tip"):
            self.verify(push_ref="refs/heads/ai-main", remote_sha="1" * 40)

    def test_rejects_tag_replacement(self) -> None:
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "never replace"):
            self.verify(push_ref=f"refs/tags/{TAG}", remote_sha=self.base)

    def test_rejects_lightweight_original_tag(self) -> None:
        self.git("tag", "-d", PRIOR)
        self.git("tag", PRIOR, self.base)
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "must be annotated"):
            self.verify()

    def test_rejects_missing_platform(self) -> None:
        del self.receipt["platforms"]["android"]
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "platforms must have exactly"):
            self.verify()

    def test_rejects_changed_artifact_bytes(self) -> None:
        (self.evidence / "windows.exe").write_bytes(b"other build")
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "windows artifact SHA-256 mismatch"):
            self.verify()

    def test_rejects_failed_report_with_matching_checksum(self) -> None:
        reports = {
            "result.log": "Ran 1 test in 0.001s\n\nFAILED (failures=1)\n",
            "result.xml": '<testsuite failures="1"><testcase><failure/></testcase></testsuite>',
            "result.json": '{"reconciled": {"passed": 1, "failed": 1, "errors": 0}}',
        }
        for filename, report in reports.items():
            with self.subTest(filename=filename):
                self.receipt["platforms"]["windows"]["tests"] = [self.evidence_file(filename, report)]
                self.receipt["review"] = self.review_file()
                with self.assertRaisesRegex(snapshot_correction.CorrectionError, "zero failures"):
                    self.verify()

    def test_rejects_incomplete_test_totals(self) -> None:
        report = self.evidence_file("result.json", '{"reconciled": {"passed": 1}}')
        self.receipt["platforms"]["macos"]["tests"] = [report]
        self.receipt["review"] = self.review_file()
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "invalid basic test totals"):
            self.verify()

    def test_rejects_missing_changed_product_input(self) -> None:
        self.receipt["platforms"]["macos"]["source_files"] = {}
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "macos source receipt is missing changed inputs"):
            self.verify()

    def test_rejects_stale_product_input(self) -> None:
        self.receipt["platforms"]["android"]["source_files"]["src/game.cpp"] = hashlib.sha256(b"original\n").hexdigest()
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "android source SHA-256 mismatch"):
            self.verify()

    def test_rejects_review_for_another_tree(self) -> None:
        self.receipt["review"] = self.review_file(tree="0" * 40)
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "review does not approve"):
            self.verify()

    def test_rejects_replaced_evidence_after_review(self) -> None:
        self.receipt["platforms"]["windows"]["runtime"] = self.evidence_file("different.log", "other run")
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "does not approve the exact platform receipts"):
            self.verify()

    def test_checks_remote_annotated_tag_object_and_head(self) -> None:
        remote = self.root.parent / "remote"
        self.git("clone", "--bare", ".", str(remote))
        subprocess.check_call(["git", "update-ref", "refs/heads/ai-main", self.base], cwd=remote)
        self.assertEqual(self.verify(remote=str(remote)), TASK)
        subprocess.check_call(["git", "update-ref", f"refs/tags/{PRIOR}", self.base], cwd=remote)
        with self.assertRaisesRegex(snapshot_correction.CorrectionError, "annotated tag differs from the remote"):
            self.verify(remote=str(remote))
