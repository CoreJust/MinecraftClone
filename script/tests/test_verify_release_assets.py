"""Contracts for read-only verification of published snapshot release assets."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from script.ci import verify_release_assets


COMMIT = "a" * 40
TAG = "ai/EarlyDev/0.1.0/7_26.10.18"
REPOSITORY = "CoreJust/MinecraftClone"


class FakeGitHub:
    def __init__(self, responses: dict[tuple[str, ...], object], downloads: dict[str, bytes]) -> None:
        self.responses = responses
        self.downloads = downloads
        self.json_calls: list[tuple[str, ...]] = []
        self.download_calls: list[tuple[str, str, str]] = []

    def json(self, arguments):
        key = tuple(arguments)
        self.json_calls.append(key)
        if key not in self.responses:
            raise AssertionError(f"unexpected fake GitHub request: {key}")
        return self.responses[key]

    def download(self, repository: str, tag: str, name: str, destination: Path) -> None:
        self.download_calls.append((repository, tag, name))
        if name not in self.downloads:
            raise AssertionError(f"unexpected fake GitHub download: {name}")
        (destination / name).write_bytes(self.downloads[name])


class VerifyReleaseAssetsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        project = self.root / "src/shared/include/shared/ProjectInfo.hpp"
        project.parent.mkdir(parents=True)
        project.write_text(
            'constexpr std::string_view MAJOR_VERSION_NAME{ "EarlyDev" };\n'
            'constexpr std::string_view MINOR_VERSION_NAME{ "Initiation" };\n'
            "constexpr ProjectVersion PROJECT_VERSION{\n"
            "    .epoch = 0,\n"
            "    .major = 1,\n"
            "    .minor = 0,\n"
            "    .patch = 7,\n"
            "};\n",
            encoding="utf-8",
        )
        (self.root / "vcpkg.json").write_text('{"version-string": "0.1.0"}\n', encoding="utf-8")
        self.payloads = {
            "macos": b"macos archive",
            "windows": b"windows archive",
            "android": b"android apk",
        }
        self.evidence = [
            self.write_evidence("macos", "minecraftclone-0.1.0-3-macos-arm64.tar.gz"),
            self.write_evidence("windows", "minecraftclone-0.1.0-3-windows-x86_64.zip"),
            self.write_evidence("android", "minecraftclone-0.1.0-3-android-arm64-v8a.apk", android=True),
        ]

    def tearDown(self):
        self.temporary.cleanup()

    def write_evidence(self, platform: str, name: str, android: bool = False) -> Path:
        payload = self.payloads[platform]
        artifact = {
            "name": name,
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        }
        value = {
            "schema": 1,
            "platform": platform,
            "version": "0.1.0:7",
            "source_commit": COMMIT,
        }
        if android:
            value.update({
                "kind": "android-apk-evidence",
                "apk": artifact,
                "signing": {"classification": "development", "identity": "not asserted"},
                "source_exactness": "exact",
            })
        else:
            value["artifact"] = artifact
        output = self.root / f"{platform}-evidence.json"
        output.write_text(json.dumps(value), encoding="utf-8")
        return output

    def github(self, annotated: bool = True) -> FakeGitHub:
        tag_object = "b" * 40
        ref_type = "tag" if annotated else "commit"
        ref_sha = tag_object if annotated else COMMIT
        ref_endpoint = ("api", f"repos/{REPOSITORY}/git/ref/tags/ai%2FEarlyDev%2F0.1.0%2F7_26.10.18")
        responses: dict[tuple[str, ...], object] = {
            ref_endpoint: {
                "ref": f"refs/tags/{TAG}",
                "object": {"type": ref_type, "sha": ref_sha},
            },
        }
        if annotated:
            responses[("api", f"repos/{REPOSITORY}/git/tags/{tag_object}")] = {
                "sha": tag_object,
                "object": {"type": "commit", "sha": COMMIT},
            }
        downloads: dict[str, bytes] = {}
        release_assets = []
        for evidence_path in self.evidence:
            expected = verify_release_assets.load_expected_asset(evidence_path)
            payload = self.payloads[expected.platform]
            checksum = f"{expected.sha256}  {expected.name}\n".encode()
            for name, contents in ((expected.name, payload), (f"{expected.name}.sha256", checksum)):
                downloads[name] = contents
                release_assets.append({
                    "name": name,
                    "size": len(contents),
                    "digest": f"sha256:{hashlib.sha256(contents).hexdigest()}",
                    "url": f"https://github.example/assets/{name}",
                })
        responses[(
            "release",
            "view",
            TAG,
            "--repo",
            REPOSITORY,
            "--json",
            "tagName,isDraft,isPrerelease,body,databaseId,id,url,assets",
        )] = {
            "tagName": TAG,
            "isDraft": False,
            "isPrerelease": True,
            "body": "Snapshot release notes\n",
            "databaseId": 42,
            "id": "RE_fixture",
            "url": "https://github.example/release",
            "assets": release_assets,
        }
        return FakeGitHub(responses, downloads)

    def test_annotated_tag_and_exact_downloaded_bytes_produce_bound_record(self):
        (self.root / "src/shared/include/shared/ProjectInfo.hpp").unlink()
        github = self.github(annotated=True)
        output = self.root / "verification.json"
        result = verify_release_assets.verify_release(
            REPOSITORY,
            TAG,
            COMMIT,
            self.evidence,
            output,
            github,
            root=self.root,
        )
        self.assertEqual(result, json.loads(output.read_text()))
        self.assertEqual(result["tag"]["object_type"], "tag")
        self.assertEqual(result["tag"]["commit"], COMMIT)
        self.assertEqual(result["release"]["database_id"], 42)
        self.assertEqual([item["platform"] for item in result["assets"]], list(verify_release_assets.PLATFORMS))
        self.assertEqual(len(github.download_calls), 6)
        self.assertTrue(all(call[:2] == (REPOSITORY, TAG) for call in github.download_calls))
        self.assertEqual(result["verification"]["status"], "passed")
        self.assertIn("not executed or extracted", result["verification"]["scope"])

    def test_lightweight_tag_resolves_directly_to_expected_commit(self):
        github = self.github(annotated=False)
        identity = verify_release_assets.resolve_tag(REPOSITORY, TAG, github)
        self.assertEqual(identity["object_type"], "commit")
        self.assertEqual(identity["commit"], COMMIT)
        self.assertEqual(len(github.json_calls), 1)

    def test_hash_mismatch_rejects_release_without_writing_result(self):
        github = self.github()
        selected = verify_release_assets.load_expected_asset(self.evidence[0]).name
        github.downloads[selected] = b"tampered byte"
        self.assertEqual(len(github.downloads[selected]), len(self.payloads["macos"]))
        output = self.root / "verification.json"
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "SHA-256 mismatch"):
            verify_release_assets.verify_release(
                REPOSITORY,
                TAG,
                COMMIT,
                self.evidence,
                output,
                github,
            )
        self.assertFalse(output.exists())

    def test_unsafe_evidence_name_and_incomplete_platform_set_are_rejected_before_github(self):
        github = self.github()
        unsafe = json.loads(self.evidence[0].read_text())
        unsafe["artifact"]["name"] = "../macos.tar.gz"
        self.evidence[0].write_text(json.dumps(unsafe), encoding="utf-8")
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "unsafe"):
            verify_release_assets.verify_release(
                REPOSITORY,
                TAG,
                COMMIT,
                self.evidence,
                self.root / "verification.json",
                github,
            )
        self.assertEqual(github.json_calls, [])

        self.evidence[0] = self.write_evidence("macos", "minecraftclone-macos.tar.gz")
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "exactly one"):
            verify_release_assets.verify_release(
                REPOSITORY,
                TAG,
                COMMIT,
                self.evidence[:2],
                self.root / "verification.json",
                github,
            )
        self.assertEqual(github.json_calls, [])

    def test_s7_windows_secondary_verifies_mac_and_android_and_marks_windows_deferred(self):
        github = self.github()
        release_key = next(key for key in github.responses if key[:2] == ("release", "view"))
        github.responses[release_key]["assets"] = [
            item for item in github.responses[release_key]["assets"]
            if not verify_release_assets.is_windows_label(item["name"])
        ]
        github.downloads = {
            name: payload for name, payload in github.downloads.items()
            if not verify_release_assets.is_windows_label(name)
        }
        output = self.root / "s7-verification.json"

        result = verify_release_assets.verify_release(
            REPOSITORY,
            TAG,
            COMMIT,
            [self.evidence[0], self.evidence[2]],
            output,
            github,
            s7_windows_secondary=True,
            root=self.root,
        )

        self.assertEqual([item["platform"] for item in result["assets"]], ["macos", "android"])
        self.assertEqual(result["deferred_platforms"][0]["platform"], "windows")
        self.assertEqual(result["deferred_platforms"][0]["status"], "deferred")
        self.assertEqual(len(github.download_calls), 4)

    def test_s7_windows_secondary_rejects_windows_assets_and_relabelled_evidence(self):
        github = self.github()
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "Windows-labelled release assets"):
            verify_release_assets.verify_release(
                REPOSITORY,
                TAG,
                COMMIT,
                [self.evidence[0], self.evidence[2]],
                self.root / "with-windows-asset.json",
                github,
                s7_windows_secondary=True,
                root=self.root,
            )

        relabelled = self.write_evidence("macos", "minecraftclone-0.1.0-3-windows-x86_64.zip")
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "cannot be relabelled"):
            verify_release_assets.load_expected_assets(
                [relabelled, self.evidence[2]],
                COMMIT,
                s7_windows_secondary=True,
            )
        self.assertEqual(github.download_calls, [])

    def test_s7_secondary_rejects_actual_other_version_missing_owner_and_other_asset_version(self):
        project = self.root / "src/shared/include/shared/ProjectInfo.hpp"
        original = project.read_text()
        for content in (original.replace(".patch = 7", ".patch = 8"),
                        original.replace(".minor = 0", ".minor = 1"), None):
            if content is None:
                project.unlink()
            else:
                project.write_text(content)
            github = self.github()
            with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "actual candidate version"):
                verify_release_assets.verify_release(REPOSITORY, TAG, COMMIT, [self.evidence[0], self.evidence[2]],
                                                     self.root / "refused.json", github,
                                                     s7_windows_secondary=True, root=self.root)
            self.assertEqual(github.json_calls, [])
        project.write_text(original)
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "release tag"):
            verify_release_assets.verify_release(REPOSITORY, "ai/EarlyDev/0.1.0/8_26.10.18", COMMIT,
                                                 [self.evidence[0], self.evidence[2]], self.root / "refused.json",
                                                 self.github(), s7_windows_secondary=True, root=self.root)
        for evidence in (self.evidence[0], self.evidence[2]):
            value = json.loads(evidence.read_text())
            value["version"] = "0.1.0:8"
            evidence.write_text(json.dumps(value))
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "actual candidate version"):
            verify_release_assets.load_expected_assets([self.evidence[0], self.evidence[2]], COMMIT,
                                                       s7_windows_secondary=True)


    def test_ci_only_android_evidence_is_not_accepted_for_a_published_release(self):
        android = json.loads(self.evidence[2].read_text())
        android["apk"]["name"] = "minecraftclone-ci-development.apk"
        self.evidence[2].write_text(json.dumps(android), encoding="utf-8")
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "CI-only"):
            verify_release_assets.load_expected_assets(self.evidence, COMMIT)

    def test_wrong_tag_commit_or_draft_release_is_rejected(self):
        github = self.github(annotated=False)
        ref_call = github.json_calls
        del ref_call[:]
        ref_endpoint = next(key for key in github.responses if key[:1] == ("api",))
        github.responses[ref_endpoint] = {
            "ref": f"refs/tags/{TAG}",
            "object": {"type": "commit", "sha": "c" * 40},
        }
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "expected source commit"):
            verify_release_assets.verify_release(
                REPOSITORY,
                TAG,
                COMMIT,
                self.evidence,
                self.root / "wrong-commit.json",
                github,
            )

        github = self.github(annotated=False)
        release_key = next(key for key in github.responses if key[:2] == ("release", "view"))
        github.responses[release_key]["isDraft"] = True
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "not a draft"):
            verify_release_assets.verify_release(
                REPOSITORY,
                TAG,
                COMMIT,
                self.evidence,
                self.root / "draft.json",
                github,
            )

    def test_unexpected_download_output_is_rejected(self):
        github = self.github()
        original_download = github.download

        def download_with_extra(repository: str, tag: str, name: str, destination: Path) -> None:
            original_download(repository, tag, name, destination)
            (destination / "unexpected.txt").write_text("unexpected", encoding="utf-8")

        github.download = download_with_extra
        with self.assertRaisesRegex(verify_release_assets.ReleaseVerificationError, "unexpected files"):
            verify_release_assets.verify_release(
                REPOSITORY,
                TAG,
                COMMIT,
                self.evidence,
                self.root / "verification.json",
                github,
            )


if __name__ == "__main__":
    unittest.main()
