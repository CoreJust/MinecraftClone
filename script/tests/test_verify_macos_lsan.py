from __future__ import annotations

import hashlib
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPT = REPOSITORY / "script/ci/verify_macos_lsan.py"


def load_module():
    spec = importlib.util.spec_from_file_location("verify_macos_lsan", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class VerifyMacosLsanTests(unittest.TestCase):
    def setUp(self):
        self.verifier = load_module()

    def test_suppression_file_accepts_only_the_three_exact_sites(self):
        valid = "\n".join(self.verifier.EXPECTED_SUPPRESSIONS) + "\n"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "macos_lsan.supp"
            path.write_text(valid, encoding="utf-8")
            self.assertEqual(self.verifier.read_suppressions(path), self.verifier.EXPECTED_SUPPRESSIONS)
            path.write_text(valid + "leak:CoreFoundation\n", encoding="utf-8")
            with self.assertRaisesRegex(self.verifier.VerificationError, "exactly the three"):
                self.verifier.read_suppressions(path)

    def test_sanitizer_environment_keeps_leak_detection_and_exact_suppression_path(self):
        suppression = Path("script/ci/macos_lsan.supp")
        environment = {
            "ASAN_OPTIONS": "detect_leaks=1",
            "LSAN_OPTIONS": f"suppressions={suppression}:print_suppressions=1",
        }
        self.verifier.validate_sanitizer_environment(environment, suppression)
        with self.assertRaisesRegex(self.verifier.VerificationError, "detect_leaks=1"):
            self.verifier.validate_sanitizer_environment(
                {**environment, "ASAN_OPTIONS": "detect_leaks=0"}, suppression
            )

    def test_leak_frame_predicate_is_bound_to_one_4096_byte_allocation(self):
        valid = (
            "Direct leak of 4096 byte(s) in 1 object(s) allocated from:\n"
            "    #1 createApplicationLeak\n"
            "    #2 renderAndLeak\n\n"
        )
        self.assertIn(
            "renderAndLeak",
            self.verifier.application_leak_block(valid, ("createApplicationLeak", "renderAndLeak")),
        )
        split_blocks = (
            "Direct leak of 4096 byte(s) in 1 object(s) allocated from:\n"
            "    #1 createApplicationLeak\n\n"
            "Direct leak of 64 byte(s) in 1 object(s) allocated from:\n"
            "    #1 renderAndLeak\n\n"
        )
        with self.assertRaisesRegex(self.verifier.VerificationError, "4096-byte application leak"):
            self.verifier.application_leak_block(
                split_blocks, ("createApplicationLeak", "renderAndLeak")
            )

    def test_callback_control_requires_observation_and_successful_teardown(self):
        output = "\n".join(
            (
                "audio callback observed: yes",
                "audio callback succeeded: yes",
                "AudioComponentInstanceNew: OSStatus=0 (success)",
                "AudioUnitSetProperty: OSStatus=0 (success)",
                "AudioUnitInitialize: OSStatus=0 (success)",
                "AudioOutputUnitStart: OSStatus=0 (success)",
                "AudioOutputUnitStop: OSStatus=0 (success)",
                "AudioUnitUninitialize: OSStatus=0 (success)",
                "AudioComponentInstanceDispose: OSStatus=0 (success)",
                "Direct leak of 4096 byte(s) in 1 object(s) allocated from:",
                "    #1 createApplicationLeak",
                "    #2 renderAndLeak",
                "SUMMARY: AddressSanitizer: 4096 byte(s) leaked in 1 allocation(s).",
            )
        )
        result = self.verifier.verify_audio_callback_control(1, output)
        self.assertTrue(result["callback_observed"])
        self.assertTrue(result["callback_succeeded"])
        self.assertEqual(result["bytes"], 4096)
        with self.assertRaisesRegex(self.verifier.VerificationError, "AudioOutputUnitStop"):
            self.verifier.verify_audio_callback_control(
                1,
                output.replace("AudioOutputUnitStop: OSStatus=0 (success)\n", ""),
            )
        with self.assertRaisesRegex(self.verifier.VerificationError, "Audio callback did not succeed"):
            self.verifier.verify_audio_callback_control(
                1, output.replace("audio callback succeeded: yes", "audio callback succeeded: no")
            )
        with self.assertRaisesRegex(self.verifier.VerificationError, "AudioUnitInitialize"):
            self.verifier.verify_audio_callback_control(
                1,
                output.replace(
                    "AudioUnitInitialize: OSStatus=0 (success)",
                    "AudioUnitInitialize: OSStatus=-1 (failure)",
                ),
            )

    def test_clean_control_rejects_reported_leaks(self):
        self.assertEqual(self.verifier.verify_clean_control(0, "")["return_code"], 0)
        with self.assertRaisesRegex(self.verifier.VerificationError, "reported a leak"):
            self.verifier.verify_clean_control(0, "ERROR: LeakSanitizer: detected memory leaks")

    def test_last_test_summary_preserves_suppression_hits_and_unsuppressed_stack(self):
        log = """Start testing: Jul 1 00:00 UTC
1/2 Test: Clean.test
Output:
Suppressions used:
  count      bytes template
      2        128 AMCP::Utility::Dispatch_Queue::install_mig_server
      1       4096 AutoreleasePoolPage::autoreleaseNoPage
      3        384 __CFTSDGetTable
-----------------------------------------------------
Test Passed.
2/2 Test: Noisy.test
Output:
==42==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 64 byte(s) in 1 object(s) allocated from:
    #0 malloc
    #1 systemPreferences
SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s).
Test Failed.
"""
        raw = log.replace("\n", "\r\n").encode("utf-8")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "LastTest.log"
            path.write_bytes(raw)
            report = self.verifier.summarize_last_test_log(path)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["tests"], 2)
        self.assertEqual(report["last_test_log_bytes"], len(raw))
        self.assertEqual(report["last_test_log_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(
            report["suppression_hits"]["AMCP::Utility::Dispatch_Queue::install_mig_server"],
            {"count": 2, "bytes": 128},
        )
        self.assertEqual(report["unsuppressed_leak_reports"][0]["test"], "Noisy.test")
        self.assertIn("systemPreferences", report["unsuppressed_leak_reports"][0]["diagnostics"])

    def test_last_test_summary_fails_closed_without_test_sections(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "LastTest.log"
            path.write_text("Start testing\n", encoding="utf-8")
            with self.assertRaisesRegex(self.verifier.VerificationError, "no test sections"):
                self.verifier.summarize_last_test_log(path)


if __name__ == "__main__":
    unittest.main()
