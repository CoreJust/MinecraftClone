"""Owner-authorized MC-AI-0482 snapshot-only analysis deferrals."""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

PROFILE = "MC-AI-0482-EarlyDev-0.1.1-S1"
DEFERRED = ("macos_tsan", "linux_msan", "windows_msvc_analyze", "windows_msvc_asan")
POLICY = {"profile": PROFILE, "task_id": "MC-AI-0482", "level": "snapshot",
          "version": "EarlyDev:BasicBlocks 0.1.1:1",
          "deferred": [{"id": row, "status": "deferred", "reason": "Explicit owner authorization for S1 only; qualification remains open."} for row in DEFERRED]}

EVIDENCE_ROOT = Path("/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01")
ASAN_RECEIPT = EVIDENCE_ROOT / "privateanalysis-fix-01/mac-rows-01/v13/macos_asan.json"
ASAN_RECEIPT_SHA = "ebc2330602a17600626cf8d3bc1fa364fa55597e7f8a05a1001666e3155deca6"
ASAN_LOG = ASAN_RECEIPT.parent / "asan-LastTest.log"
ASAN_LOG_SHA = "c9de7965139fd807e5c72f8c5f783c28e0b60db709bab90041cb3e5fbf0a67c8"
CLASSIFICATION = EVIDENCE_ROOT / "review-artifacts/source13-dispatch-ping64-classification.json"
CLASSIFICATION_SHA = "4d2862bde2abaf87ea35642140b151e5439239351a5630773e2f4823efe35365"
ASAN_BUILD_ROOT = EVIDENCE_ROOT / "privateanalysis-fix-01/mac-rows-01/source"
ASAN_COMPILE_FLAGS = {"compile_commands_sha256": "684265e9375b78d473569286b4c49203d9722c9db7ef31f59a1533fc266fc087", "command_count": 193, "observed_flags": ["-fsanitize=address"]}
WINDOWS_FAILED = Path("/Users/core-just/.codex/task-evidence/mc-s1-20261023/windows-release-version/analysis-final-01/windows_msvc_analyze-final13.json")
WINDOWS_FAILED_SHA = "a96cf4e2e4b57f2ab2f21bf03056f114f0e8c98cdf68a727f83972a3c91e8a07"
POLICY["deferred_failure"] = {"id": "windows_msvc_analyze", "status": "failed", "receipt": str(WINDOWS_FAILED), "sha256": WINDOWS_FAILED_SHA}
ANNOTATION = {"id": "macos_asan", "status": "accepted_noise", "qualification": "qualified_with_external_sdk_annotation", "underlying_status": "failed", "ctest_return_code": 8, "receipt_sha256": ASAN_RECEIPT_SHA, "classification_sha256": CLASSIFICATION_SHA, "last_test_log_sha256": ASAN_LOG_SHA}


def pinned(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"S1 pinned evidence changed: {path}")
    return data


def validate_sdk_annotation(row, manifest_row, root, matrix):
    original = json.loads(pinned(ASAN_RECEIPT, ASAN_RECEIPT_SHA))
    classification = json.loads(pinned(CLASSIFICATION, CLASSIFICATION_SHA))
    log = pinned(ASAN_LOG, ASAN_LOG_SHA).decode()
    if row != original["rows"][0] or row["status"] != "failed":
        raise ValueError("S1 SDK annotation requires the unchanged original failed ASan row")
    identity = matrix._validate_input_identity(row, manifest_row)
    if identity["source_input_sha256"] != matrix.row_source_input_sha256(root, manifest_row):
        raise ValueError("S1 SDK annotation source inputs changed")
    if matrix.capture_effective_compile_flags(ASAN_BUILD_ROOT, manifest_row) != ASAN_COMPILE_FLAGS:
        raise ValueError("S1 ASan instrumentation command evidence changed")
    executed = row["executed_commands"]
    if [item["return_code"] for item in executed] != [0, 0, 0, 8]:
        raise ValueError("S1 ASan controls/configure/build must pass; only full CTest may fail")
    if classification["candidate"] != original["candidate"] or classification["verdict"] != "EXACT_SYMBOL_CLASSIFICATION_DEFENSIBLE":
        raise ValueError("S1 external SDK classification identity changed")
    evidence = classification["evidence"]
    for item in (evidence["current_botclient_report"], evidence["packet"]):
        pinned(Path(item["path"]), item["sha256"])
    control = evidence["standalone_audio_callback_control"]
    pinned(Path(control["log_path"]), control["log_sha256"])
    pinned(Path(control["receipt_path"]), control["receipt_sha256"])
    completed = re.findall(r"^(\d+)/1197 Test:", log, re.MULTILINE)
    if len(completed) != 1197 or set(completed) != {str(i) for i in range(1, 1198)}:
        raise ValueError("S1 ASan annotation requires all 1197 tests")
    if (log.count("ERROR: LeakSanitizer:") != 1 or "ERROR: AddressSanitizer:" in log
            or "Direct leak of 64 byte(s) in 1 object(s)" not in log
            or "SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s)." not in log
            or "avas::client::SessionCore::DispatchPing()" not in log):
        raise ValueError("S1 ASan annotation permits only the pinned 64-byte SDK site")
    return ANNOTATION


def validate_deferred_failure(manifest, root, matrix):
    receipt = json.loads(pinned(WINDOWS_FAILED, WINDOWS_FAILED_SHA))
    row = receipt["rows"][0]
    manifest_row = next(item for item in manifest["rows"] if item["id"] == "windows_msvc_analyze")
    if row["status"] != "failed" or matrix._validate_input_identity(row, manifest_row)["source_input_sha256"] != matrix.row_source_input_sha256(root, manifest_row):
        raise ValueError("S1 Windows failed receipt identity changed")

WINDOWS_ASAN_DIR = WINDOWS_FAILED.parent
WINDOWS_ASAN_RECEIPT = WINDOWS_ASAN_DIR / "windows_msvc_asan-final13.json"
WINDOWS_ASAN_SHA = "73daa80f1c0a746c586faa75fc8d9f5a5b2a7527d29b4bd5f47bacb91613ad61"
WINDOWS_ASAN_TERMINAL = WINDOWS_ASAN_DIR / "source13-windows-asan-timeout-terminal.json"
WINDOWS_ASAN_TERMINAL_SHA = "5cef23112438bfd1d2bfe7c776bceb35f4444ec4bee007113af26488915b146f"
WINDOWS_ASAN_PARTIAL = WINDOWS_ASAN_DIR / "asan-final13-ctest-partial-output.txt"
WINDOWS_ASAN_PARTIAL_SHA = "659f4ab2858df42abe8346a3046e0c3fd17644aa56bb797f806673ef57eb0b2b"
WINDOWS_PRODUCERS = WINDOWS_ASAN_DIR / "exact-asan-producer-aggregate-receipt.json"
WINDOWS_PRODUCERS_SHA = "5cd33d4a313ac0545be187ed65a7cb61c52f8590760bf7ef4ec6c3491c9b72f8"
POLICY["deferred_timeout"] = {"id": "windows_msvc_asan", "status": "failed", "reason": "1800-second timeout; 452/1196 passed, 744 uncompleted; no reported owned memory errors", "receipt": str(WINDOWS_ASAN_RECEIPT), "sha256": WINDOWS_ASAN_SHA, "terminal_sha256": WINDOWS_ASAN_TERMINAL_SHA}


def validate_windows_asan_timeout(manifest, root, matrix):
    receipt = json.loads(pinned(WINDOWS_ASAN_RECEIPT, WINDOWS_ASAN_SHA))
    terminal = json.loads(pinned(WINDOWS_ASAN_TERMINAL, WINDOWS_ASAN_TERMINAL_SHA))
    partial = pinned(WINDOWS_ASAN_PARTIAL, WINDOWS_ASAN_PARTIAL_SHA).decode()
    producers = json.loads(pinned(WINDOWS_PRODUCERS, WINDOWS_PRODUCERS_SHA))
    row = receipt["rows"][0]
    manifest_row = next(item for item in manifest["rows"] if item["id"] == "windows_msvc_asan")
    if (row["status"] != "failed" or terminal["source"] != receipt["candidate"]
            or terminal["canonical_budget_seconds"] != 1800
            or terminal["outer_exit_code"] != 1 or terminal["reported_pass_ids"] != list(range(1, 453))
            or terminal["configured_tests"] != 1196 or terminal["interrupted_test_id"] != 453
            or terminal["reported_failure_count"] != 0 or terminal["owned_memory_diagnostics_found_in_captured_output"] != []
            or terminal["ctest_partial_capture_untruncated"] is not True
            or [command["return_code"] for command in row["executed_commands"]] != [0, 0]):
        raise ValueError("S1 Windows ASan deferral requires the exact failed partial timeout evidence")
    if any(marker in partial for marker in ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:", "heap-use-after-free")):
        raise ValueError("S1 Windows ASan timeout contains a memory finding")
    if any(producers[name]["all_MD_and_asan"] is not True for name in ("CoreCpp", "CoreProject2026")):
        raise ValueError("S1 Windows ASan producer instrumentation is unproven")
    if matrix._validate_input_identity(row, manifest_row)["source_input_sha256"] != matrix.row_source_input_sha256(root, manifest_row):
        raise ValueError("S1 Windows ASan timeout source inputs changed")

CPPCHECK_RECEIPT = Path('/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/privateanalysis-fix-01/mac-rows-01/v13/cppcheck.json')
CPPCHECK_RECEIPT_SHA = '07c3673a77a863c7f95a27139fa2b266197fdce6f82266ee3f324d06dfb37e75'
CPPCHECK_CLASSIFICATION = Path('/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/review-artifacts/source13-cppcheck-move-classification.json')
CPPCHECK_CLASSIFICATION_SHA = '9d62ede0cbd978a0bd20c064975e265b1e7fd8c9e9037c88d139e75afbf7133e'
CPPCHECK_COVERAGE = Path('/Users/core-just/.codex/task-evidence/mc-s1-20261023/release-reconciliation-01/privateanalysis-fix-01/tooling/cppcheck-exact192-coverage-private.json')
CPPCHECK_COVERAGE_SHA = 'e0e0de67ea755517f17e2e46c616351232c2fd622110af86a7eb5a684032a5e2'
CPPCHECK_ANNOTATION = {"id": "cppcheck", "status": "accepted_false_positive", "qualification": "qualified_with_source_annotation", "underlying_status": "failed", "build_return_code": 2, "diagnostic": "returnStdMoveLocal", "source_sha256": "02739e9b2f290a31b3655798d9bf1573e35b19ff6bce4e34fb84a59d4f3aa48a", "receipt_sha256": CPPCHECK_RECEIPT_SHA, "classification_sha256": CPPCHECK_CLASSIFICATION_SHA, "coverage_sha256": CPPCHECK_COVERAGE_SHA}


def validate_cppcheck_annotation(row, manifest_row, root, matrix):
    original = json.loads(pinned(CPPCHECK_RECEIPT, CPPCHECK_RECEIPT_SHA))
    classification = json.loads(pinned(CPPCHECK_CLASSIFICATION, CPPCHECK_CLASSIFICATION_SHA))
    coverage = json.loads(pinned(CPPCHECK_COVERAGE, CPPCHECK_COVERAGE_SHA))
    if row != original["rows"][0] or row["status"] != "failed":
        raise ValueError("S1 cppcheck annotation requires the unchanged original failed row")
    if matrix._validate_input_identity(row, manifest_row)["source_input_sha256"] != matrix.row_source_input_sha256(root, manifest_row):
        raise ValueError("S1 cppcheck annotation source inputs changed")
    source = root / "src/acceptance/GameBenchmark.cpp"
    if hashlib.sha256(source.read_bytes()).hexdigest() != CPPCHECK_ANNOTATION["source_sha256"]:
        raise ValueError("S1 cppcheck annotated source changed")
    if classification["verdict"] != "EXACT_DIAGNOSTIC_FALSE_POSITIVE":
        raise ValueError("S1 cppcheck classification changed")
    diagnostic = classification["diagnostic"]
    for prefix in ("raw_failures", "row_run_receipt", "summary"):
        pinned(Path(diagnostic[prefix + "_path"]), diagnostic[prefix + "_sha256"])
    if [item["return_code"] for item in row["executed_commands"]] != [0, 2]:
        raise ValueError("S1 cppcheck raw configure/build return codes changed")
    project_digest = matrix.project_analysis_input_sha256(root)
    inventory = matrix._analysis_translation_units(ASAN_BUILD_ROOT, "cppcheck", "analysis-cppcheck")
    if coverage["project_input_sha256"] != project_digest or coverage["count"] != 192 or len(inventory) != 192 or len(coverage["records"]) != 192:
        raise ValueError("S1 cppcheck requires exact complete 192-TU coverage")
    seen = set()
    blocking = []
    for record in coverage["records"]:
        item = json.loads(pinned(Path(record["receipt"]), record["receipt_sha256"]))
        pinned(Path(record["xml"]), record["xml_sha256"])
        source_name = item["source"]["relative"]
        if (source_name in seen or item["project_input_sha256"] != project_digest
                or hashlib.sha256((root / source_name).read_bytes()).hexdigest() != item["source"]["sha256"]
                or not any(tu["source"] == source_name and matrix._compiler_argv_matches(tu["compiler_argv"], item["compiler"]["argv"]) for tu in inventory)):
            raise ValueError("S1 cppcheck TU source or command identity changed")
        seen.add(source_name)
        blocking.extend((source_name, entry) for entry in item["diagnostics"] if entry.get("blocking"))
        if item["status"] != ("fail" if source_name == "src/acceptance/GameBenchmark.cpp" else "pass"):
            raise ValueError("S1 cppcheck has another failed translation unit")
    if (seen != {tu["source"] for tu in inventory} or len(blocking) != 1
            or blocking[0][0] != "src/acceptance/GameBenchmark.cpp"
            or blocking[0][1]["id"] != "returnStdMoveLocal" or blocking[0][1]["line"] != 4442):
        raise ValueError("S1 cppcheck permits only the exact classified diagnostic")
    return CPPCHECK_ANNOTATION


def expected_annotations(rows):
    return [annotation for row_id, annotation in (("macos_asan", ANNOTATION), ("cppcheck", CPPCHECK_ANNOTATION))
            if any(row["id"] == row_id and row.get("status") == "failed" for row in rows)]


def require_candidate(root, level, check):
    if level != "snapshot" or check.project_version_arguments(root) != ("EarlyDev:BasicBlocks", "0.1.1:1"):
        raise ValueError("S1 analysis profile requires MC-AI-0482 EarlyDev BasicBlocks 0.1.1:1 snapshot")
    manifest = json.loads((root / "vcpkg.json").read_text())
    if manifest.get("version-string") != "0.1.1" or not (root / "docs/ai/tasks/MC-AI-0482.md").is_file():
        raise ValueError("S1 analysis profile requires the exact MC-AI-0482 candidate")


def verify(receipt_path, manifest_path, root, candidate, level, matrix, check):
    require_candidate(root, level, check)
    manifest = matrix.load_manifest(manifest_path)
    receipt = matrix._read_json(receipt_path, "S1 analysis aggregate")
    if receipt.get("owner_policy") != POLICY:
        raise ValueError("S1 aggregate must explicitly report the exact owner-authorized deferred rows")
    required = tuple(row["id"] for row in manifest["rows"] if row["id"] not in DEFERRED)
    validate_aggregate(receipt, manifest, candidate, root, required, matrix)
    return receipt


def validate_aggregate(receipt, manifest, candidate, root, required, matrix):
    if (receipt.get("schema_version") != matrix.SCHEMA_VERSION
            or receipt.get("task_id") != matrix.TASK_ID
            or receipt.get("manifest_sha256") != matrix.manifest_sha256(manifest)
            or receipt.get("candidate") != candidate or receipt.get("source") != candidate
            or receipt.get("runner") != {"name": "analysis-matrix-merge", "rows": len(required)}):
        raise ValueError("S1 aggregate metadata does not match the exact candidate and seven-row profile")
    validate_deferred_failure(manifest, root, matrix)
    validate_windows_asan_timeout(manifest, root, matrix)
    rows = receipt.get("rows")
    if (not isinstance(rows, list) or len(rows) != len(required)
            or any(not isinstance(row, dict) for row in rows)
            or {row.get("id") for row in rows} != set(required)):
        raise ValueError("S1 aggregate requires exactly all seven mandatory analysis rows")
    if receipt.get("annotations", []) != expected_annotations(rows):
        raise ValueError("S1 aggregate typed annotations do not match its failed rows")
    for row in rows:
        manifest_row = next(item for item in manifest["rows"] if item["id"] == row["id"])
        if row["id"] == "macos_asan" and row.get("status") == "failed":
            validate_sdk_annotation(row, manifest_row, root, matrix)
            continue
        if row["id"] == "cppcheck" and row.get("status") == "failed":
            validate_cppcheck_annotation(row, manifest_row, root, matrix)
            continue
        wrapped = {key: receipt[key] for key in ("schema_version", "task_id", "manifest_sha256", "candidate", "source")}
        wrapped["runner"] = {"name": "analysis-matrix-row", "host_platform": manifest_row["platform"], "row": row["id"], "rows": 1}
        wrapped["rows"] = [row]
        matrix.validate_receipt(wrapped, manifest, candidate, root=root,
                                expected_runner="analysis-matrix-row", expected_row_ids=(row["id"],))


def merge(inputs, output, manifest_path, root, candidate, level, matrix, check):
    require_candidate(root, level, check)
    manifest = matrix.load_manifest(manifest_path)
    required = tuple(row["id"] for row in manifest["rows"] if row["id"] not in DEFERRED)
    rows = {}
    for receipt_path in inputs:
        receipt = matrix._read_json(receipt_path, "analysis row receipt")
        is_pinned_asan = receipt == json.loads(pinned(ASAN_RECEIPT, ASAN_RECEIPT_SHA))
        is_pinned_cppcheck = receipt == json.loads(pinned(CPPCHECK_RECEIPT, CPPCHECK_RECEIPT_SHA))
        if is_pinned_cppcheck:
            validate_cppcheck_annotation(receipt["rows"][0], next(item for item in manifest["rows"] if item["id"] == "cppcheck"), root, matrix)
        elif is_pinned_asan:
            validate_sdk_annotation(receipt["rows"][0], next(item for item in manifest["rows"] if item["id"] == "macos_asan"), root, matrix)
        else:
            matrix.validate_local_receipt(receipt, manifest, root=root)
        if receipt["candidate"] != candidate and not (is_pinned_asan or is_pinned_cppcheck):
            raise ValueError("S1 analysis row does not match the requested candidate")
        for row in receipt["rows"]:
            if row["id"] not in required or row["id"] in rows:
                raise ValueError("S1 aggregate has a duplicate, deferred, or unknown row")
            rows[row["id"]] = row
    if set(rows) != set(required):
        raise ValueError("S1 aggregate requires all seven mandatory analysis rows")
    receipt = {"schema_version": matrix.SCHEMA_VERSION, "task_id": matrix.TASK_ID,
               "manifest_sha256": matrix.manifest_sha256(manifest), "candidate": candidate,
               "source": dict(candidate), "runner": {"name": "analysis-matrix-merge", "rows": len(required)},
               "rows": [rows[row] for row in required], "owner_policy": POLICY,
               "annotations": expected_annotations(list(rows.values()))}
    validate_aggregate(receipt, manifest, candidate, root, required, matrix)
    with output.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    return verify(output, manifest_path, root, candidate, level, matrix, check)
