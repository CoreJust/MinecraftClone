from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from script.ci import s1_analysis_profile as profile


class S1AnalysisProfileTests(unittest.TestCase):
    def fixture(self, root):
        (root / 'docs/ai/tasks').mkdir(parents=True)
        (root / 'docs/ai/tasks/MC-AI-0482.md').write_text('# MC-AI-0482\n')
        (root / 'vcpkg.json').write_text('{"version-string":"0.1.1"}')
        return SimpleNamespace(project_version_arguments=lambda _: ('EarlyDev:BasicBlocks', '0.1.1:1'))

    def test_profile_rejects_minor_and_other_snapshot_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            check = self.fixture(root)
            with self.assertRaisesRegex(ValueError, 'snapshot'):
                profile.require_candidate(root, 'minor', check)
            check.project_version_arguments = lambda _: ('EarlyDev:BasicBlocks', '0.1.1:2')
            with self.assertRaisesRegex(ValueError, 'snapshot'):
                profile.require_candidate(root, 'snapshot', check)

    def test_profile_requires_task_and_package_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            check = self.fixture(root)
            (root / 'vcpkg.json').write_text('{"version-string":"0.1.2"}')
            with self.assertRaisesRegex(ValueError, 'exact MC-AI-0482'):
                profile.require_candidate(root, 'snapshot', check)
            (root / 'vcpkg.json').write_text('{"version-string":"0.1.1"}')
            (root / 'docs/ai/tasks/MC-AI-0482.md').unlink()
            with self.assertRaisesRegex(ValueError, 'exact MC-AI-0482'):
                profile.require_candidate(root, 'snapshot', check)

    def test_nine_row_profile_still_uses_strict_candidate_row_validator(self):
        from script import ai_analysis_matrix as matrix
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            check = self.fixture(root)
            manifest = matrix.load_manifest(matrix.DEFAULT_MANIFEST)
            candidate = {'head': 'a' * 40, 'tree': 'b' * 40}
            required = tuple(row['id'] for row in manifest['rows'] if row['id'] not in profile.DEFERRED)
            receipt = {'owner_policy': profile.POLICY, 'schema_version': matrix.SCHEMA_VERSION,
                       'task_id': matrix.TASK_ID, 'manifest_sha256': matrix.manifest_sha256(manifest),
                       'candidate': candidate, 'source': candidate,
                       'runner': {'name': 'analysis-matrix-merge', 'rows': 7},
                       'rows': [{'id': row} for row in required]}
            fake = SimpleNamespace(load_manifest=lambda _: manifest, SCHEMA_VERSION=matrix.SCHEMA_VERSION,
                                   TASK_ID=matrix.TASK_ID, manifest_sha256=matrix.manifest_sha256,
                                   _read_json=lambda *args: receipt, validate_receipt=Mock())
            with patch.object(profile, 'validate_deferred_failure'), patch.object(profile, 'validate_windows_asan_timeout'):
                profile.verify(root/'receipt', matrix.DEFAULT_MANIFEST, root, candidate, 'snapshot', fake, check)
            self.assertEqual(fake.validate_receipt.call_count, 7)
            for call in fake.validate_receipt.call_args_list:
                self.assertEqual(call.args[2], candidate)
                self.assertEqual(len(call.kwargs['expected_row_ids']), 1)
                self.assertFalse(set(call.kwargs['expected_row_ids']) & set(profile.DEFERRED))
            removed = receipt['rows'].pop()
            with patch.object(profile, 'validate_deferred_failure'), patch.object(profile, 'validate_windows_asan_timeout'), self.assertRaisesRegex(ValueError, 'exactly all seven'):
                profile.verify(root/'receipt', matrix.DEFAULT_MANIFEST, root, candidate, 'snapshot', fake, check)
            receipt['rows'].append(removed)
            with patch.object(profile, 'validate_deferred_failure'), patch.object(profile, 'validate_windows_asan_timeout'), self.assertRaises(matrix.MatrixError):
                profile.validate_aggregate(receipt, manifest, candidate, root, required, matrix)
            receipt['owner_policy'] = {}
            with self.assertRaisesRegex(ValueError, 'explicitly report'):
                profile.verify(root/'receipt', matrix.DEFAULT_MANIFEST, root, candidate, 'snapshot', fake, check)

    def test_registry_selector_keeps_other_snapshots_on_strict_verifier(self):
        from script.ci import verify_analysis_gate as gate
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = root / 'receipt.json'
            receipt.write_text('{}')
            manifest = Path(__file__).resolve().parents[1] / 'ai_analysis_matrix.json'
            candidate = {'head': 'a' * 40, 'tree': 'b' * 40}
            with patch.object(gate._PROFILE, 'require_candidate', side_effect=ValueError('S2')), \
                    patch.object(gate._MATRIX, 'candidate_identity', return_value=candidate), \
                    patch.object(gate._MATRIX, 'verify_receipt', return_value={'candidate': candidate, 'rows': []}) as strict:
                self.assertEqual(gate.main(['--root', str(root), '--receipt', str(receipt),
                                           '--manifest', str(manifest), '--s1-owner-deferred-if-applicable',
                                           '--release-level', 'snapshot']), 0)
                strict.assert_called_once()

    def test_sdk_annotation_rejects_relabelled_or_changed_failed_row(self):
        original = {'rows': [{'id': 'macos_asan', 'status': 'failed'}]}
        outputs = [json.dumps(original).encode(), b'{}', b'']
        with patch.object(profile, 'pinned', side_effect=outputs):
            with self.assertRaisesRegex(ValueError, 'unchanged original failed'):
                profile.validate_sdk_annotation({'id': 'macos_asan', 'status': 'passed'}, {}, Path('/tmp'), None)

    def test_pinned_evidence_rejects_any_byte_change(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'raw'
            target.write_bytes(b'changed owned leak')
            with self.assertRaisesRegex(ValueError, 'pinned evidence changed'):
                profile.pinned(target, '0' * 64)

    def test_windows_timeout_rejects_owned_memory_findings(self):
        candidate = {'head': 'a' * 40, 'tree': 'b' * 40}
        receipt = {'candidate': candidate, 'rows': [{'status': 'failed', 'executed_commands': [{'return_code': 0}, {'return_code': 0}]}]}
        terminal = {'source': candidate, 'canonical_budget_seconds': 1800, 'outer_exit_code': 1,
                    'reported_pass_ids': list(range(1, 453)), 'configured_tests': 1196,
                    'interrupted_test_id': 453, 'reported_failure_count': 0,
                    'owned_memory_diagnostics_found_in_captured_output': [], 'ctest_partial_capture_untruncated': True}
        producers = {name: {'all_MD_and_asan': True} for name in ('CoreCpp', 'CoreProject2026')}
        for marker in ('ERROR: AddressSanitizer: heap-use-after-free', 'runtime error: invalid access'):
            with self.subTest(marker=marker), patch.object(profile, 'pinned', side_effect=[
                    json.dumps(receipt).encode(), json.dumps(terminal).encode(), marker.encode(), json.dumps(producers).encode()]):
                with self.assertRaisesRegex(ValueError, 'memory finding'):
                    profile.validate_windows_asan_timeout({'rows': [{'id': 'windows_msvc_asan'}]}, Path('/tmp'), None)

    def test_cppcheck_annotation_rejects_changed_or_relabelled_row(self):
        original = {'rows': [{'id': 'cppcheck', 'status': 'failed', 'diagnostics': 'sole classified site'}]}
        for changed in ({'id': 'cppcheck', 'status': 'passed'},
                        {'id': 'cppcheck', 'status': 'failed', 'diagnostics': 'new owned error'}):
            with self.subTest(changed=changed), patch.object(profile, 'pinned', side_effect=[json.dumps(original).encode(), b'{}', b'{}']):
                with self.assertRaisesRegex(ValueError, 'unchanged original failed'):
                    profile.validate_cppcheck_annotation(changed, {}, Path('/tmp'), None)

    def test_typed_annotations_retain_both_failed_dispositions(self):
        annotations = profile.expected_annotations([{'id': 'macos_asan', 'status': 'failed'},
                                                    {'id': 'cppcheck', 'status': 'failed'}])
        self.assertEqual([item['status'] for item in annotations], ['accepted_noise', 'accepted_false_positive'])
        self.assertTrue(all(item['underlying_status'] == 'failed' for item in annotations))

    def test_default_matrix_still_requires_all_eleven_rows(self):
        from script import ai_analysis_matrix as matrix
        self.assertEqual(len(matrix.REQUIRED_ROW_IDS), 11)
        self.assertEqual({item['status'] for item in profile.POLICY['deferred']}, {'deferred'})
