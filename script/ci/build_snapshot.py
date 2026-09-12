#!/usr/bin/env python3
"""Build-release orchestration that preserves source and artifact provenance."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Sequence


REVISION_RE = re.compile(r"[0-9a-f]{40}")
VERSION_RE = re.compile(r"(\d+)\.(\d+)\.(\d+):(\d+)")
PROJECT_VERSION_RE = re.compile(r"\.(epoch|major|minor|patch)\s*=\s*(\d+)")
WINDOWS_DEPENDENCY_RE = re.compile(r"^\s+([A-Za-z0-9_.+-]+\.dll)\s*$", re.IGNORECASE)
WINDOWS_OS_DLLS = frozenset({
    "advapi32.dll",
    "bcrypt.dll",
    "cfgmgr32.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "d3d12.dll",
    "dxgi.dll",
    "dwmapi.dll",
    "gdi32.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "psapi.dll",
    "rpcrt4.dll",
    "setupapi.dll",
    "shell32.dll",
    "shlwapi.dll",
    "ucrtbase.dll",
    "user32.dll",
    "userenv.dll",
    "uxtheme.dll",
    "version.dll",
    "winmm.dll",
    "ws2_32.dll",
})
VENDOR_LICENSES = {
    "MoltenVK-LICENSE.txt": {
        "url": "https://raw.githubusercontent.com/KhronosGroup/MoltenVK/db445ff2042d9ce348c439ad8451112f354b8d2a/LICENSE",
        "sha256": "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
    },
    "Vulkan-Loader-LICENSE.txt": {
        "url": "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/5f157b62e333c63260d05d81bf66faa216ab0fb8/LICENSE.txt",
        "sha256": "43c0a37e6a0fa7ff3c843b3ec5a4fac84b712558ddac103fbd4c1649662a9ece",
    },
}
MICROSOFT_REDISTRIBUTION_POLICY = (
    "https://learn.microsoft.com/en-us/cpp/windows/"
    "determining-which-dlls-to-redistribute?view=msvc-170"
)
ANDROID_REQUIRED_FILES = (
    "build.gradle",
    "settings.gradle",
    "gradle.properties",
    "gradlew",
    "gradle/wrapper/gradle-wrapper.jar",
    "gradle/wrapper/gradle-wrapper.properties",
    "app/build.gradle",
    "app/src/main/AndroidManifest.xml",
    "app/src/main/res/values/styles.xml",
)
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
HUD_LICENSE_SOURCES = (
    (
        "hud/DEBUG_HUD_ATTRIBUTION.md",
        REPOSITORY_ROOT / "src/client/render/shaders/DEBUG_HUD_ATTRIBUTION.md",
    ),
    (
        "hud/TAMSYN_LICENSE.txt",
        REPOSITORY_ROOT / "src/client/render/shaders/TAMSYN_LICENSE.txt",
    ),
)


class SnapshotBuildError(RuntimeError):
    """Raised when release provenance or package inputs are incomplete."""


def run(command: Sequence[str], cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)
    except OSError as error:
        raise SnapshotBuildError(f"could not start {' '.join(command)}: {error}") from error
    output = completed.stdout + completed.stderr
    if completed.returncode:
        raise SnapshotBuildError(f"{' '.join(command)} exited {completed.returncode}: {output.strip()}")
    return output.strip()


def sha256_file(source: Path) -> str:
    digest = hashlib.sha256()
    with source.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_file(source: Path, label: str) -> Path:
    if source.is_symlink() or not source.is_file():
        raise SnapshotBuildError(f"{label} must be an existing regular file: {source}")
    return source.resolve()


def require_revision(value: str) -> str:
    if not REVISION_RE.fullmatch(value):
        raise SnapshotBuildError("source commit must be a 40-character lowercase hexadecimal revision")
    return value


def require_version(value: str) -> str:
    if not VERSION_RE.fullmatch(value):
        raise SnapshotBuildError("version must use EPOCH.MAJOR.MINOR:SNAPSHOT")
    return value


def canonical_json(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def project_version(root: Path) -> str:
    source = require_file(root / "src/shared/include/shared/ProjectInfo.hpp", "project version header")
    fields = {name: int(value) for name, value in PROJECT_VERSION_RE.findall(source.read_text(encoding="utf-8"))}
    if set(fields) != {"epoch", "major", "minor", "patch"}:
        raise SnapshotBuildError(f"could not read the complete project version from {source}")
    return f"{fields['epoch']}.{fields['major']}.{fields['minor']}:{fields['patch']}"


def write_source_identity(
    root: Path,
    expected: str | None,
    requested_ref: str | None,
    version: str,
    output: Path,
    github_output: Path | None,
) -> dict[str, Any]:
    actual_version = project_version(root)
    if version and require_version(version) != actual_version:
        raise SnapshotBuildError(f"requested version {version} does not match source version {actual_version}")
    version = actual_version
    revision = require_revision(run(["git", "rev-parse", "HEAD"], cwd=root))
    if expected is not None and require_revision(expected) != revision:
        raise SnapshotBuildError(f"checked out source {revision} does not match resolved commit {expected}")
    tree = require_revision(run(["git", "rev-parse", "HEAD^{tree}"], cwd=root))
    identity = {
        "schema": 1,
        "source_commit": revision,
        "source_tree": tree,
        "version": version,
        "version_slug": version.replace(":", "-"),
    }
    if requested_ref is not None:
        identity["requested_ref"] = requested_ref
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(canonical_json(identity), encoding="utf-8")
    if github_output is not None:
        with github_output.open("a", encoding="utf-8") as stream:
            stream.write(
                f"source_commit={revision}\n"
                f"version={version}\n"
                f"version_slug={identity['version_slug']}\n"
            )
    return identity


def download_verified(url: str, expected_sha256: str, output: Path) -> None:
    try:
        with urllib.request.urlopen(url) as response, output.open("wb") as stream:
            shutil.copyfileobj(response, stream)
    except OSError as error:
        raise SnapshotBuildError(f"could not download license {url}: {error}") from error
    actual = sha256_file(output)
    if actual != expected_sha256:
        output.unlink(missing_ok=True)
        raise SnapshotBuildError(f"SHA-256 mismatch for {url}: expected {expected_sha256}, got {actual}")


def vcpkg_copyrights(search_root: Path) -> dict[str, Path]:
    candidates = sorted(search_root.rglob("share/*/copyright"))
    if not candidates:
        raise SnapshotBuildError(f"no vcpkg copyright files found below {search_root}")
    result: dict[str, Path] = {}
    for candidate in candidates:
        source = require_file(candidate, "vcpkg copyright")
        port = candidate.parent.name
        previous = result.get(port)
        if previous is not None and previous.read_bytes() != source.read_bytes():
            raise SnapshotBuildError(f"conflicting vcpkg copyright files for {port}")
        result[port] = source
    return result


def prepare_licenses(
    project_license: Path,
    vcpkg_root: Path,
    output: Path,
    include_vendor: bool,
    hud_sources: Sequence[tuple[str, Path]] | None = None,
) -> Path:
    if output.exists() or output.is_symlink():
        raise SnapshotBuildError(f"license output already exists: {output}")
    output.mkdir(parents=True)
    project = require_file(project_license, "project license")
    (output / "MinecraftClone-LICENSE.txt").write_bytes(project.read_bytes())
    dependency_root = output / "vcpkg"
    dependency_root.mkdir()
    for port, source in vcpkg_copyrights(vcpkg_root).items():
        (dependency_root / f"{port}-copyright.txt").write_bytes(source.read_bytes())
    for relative, source in hud_sources or HUD_LICENSE_SOURCES:
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        contents = require_file(source, "HUD attribution/license").read_bytes()
        if not contents:
            raise SnapshotBuildError(f"HUD attribution/license must not be empty: {source}")
        destination.write_bytes(contents)
    if include_vendor:
        vendor_root = output / "vulkan"
        vendor_root.mkdir()
        for name, config in sorted(VENDOR_LICENSES.items()):
            download_verified(config["url"], config["sha256"], vendor_root / name)
    return output


def write_license_archive(source: Path, output: Path) -> None:
    if output.exists() or output.is_symlink():
        raise SnapshotBuildError(f"license archive already exists: {output}")
    files = sorted(path for path in source.rglob("*") if path.is_file() and not path.is_symlink())
    if not files:
        raise SnapshotBuildError(f"license directory is empty: {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for item in files:
            info = zipfile.ZipInfo(item.relative_to(source).as_posix(), date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100644) << 16
            archive.writestr(info, item.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def windows_dependencies(binary: Path) -> list[str]:
    output = run(["dumpbin", "/dependents", str(binary)])
    return sorted({match.group(1) for line in output.splitlines() if (match := WINDOWS_DEPENDENCY_RE.match(line))})


def dependency_index(search_roots: Sequence[Path]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for root in search_roots:
        if not root.exists():
            continue
        for candidate in sorted(root.rglob("*.dll")):
            source = require_file(candidate, "Windows runtime dependency")
            key = candidate.name.lower()
            previous = result.get(key)
            if previous is not None and previous.read_bytes() != source.read_bytes():
                raise SnapshotBuildError(f"ambiguous Windows runtime dependency {candidate.name}")
            result[key] = source
    return result


def windows_system_dll(name: str) -> bool:
    normalized = name.lower()
    return normalized in WINDOWS_OS_DLLS or normalized.startswith(("api-ms-win-", "ext-ms-win-"))


def resolve_windows_runtime(executable: Path, search_roots: Sequence[Path]) -> list[Path]:
    index = dependency_index(search_roots)
    queued = windows_dependencies(require_file(executable, "Windows executable"))
    resolved: dict[str, Path] = {}
    while queued:
        name = queued.pop(0)
        key = name.lower()
        if key in resolved or windows_system_dll(name):
            continue
        dependency = index.get(key)
        if dependency is None:
            raise SnapshotBuildError(f"Windows runtime dependency is neither packaged nor a system DLL: {name}")
        resolved[key] = dependency
        queued.extend(windows_dependencies(dependency))
    if not resolved:
        raise SnapshotBuildError("Windows package resolved no non-system runtime DLLs")
    return [resolved[name] for name in sorted(resolved)]


def msvc_redist_version(source: Path) -> str | None:
    parts = source.parts
    for index, part in enumerate(parts[:-1]):
        if part.lower() == "msvc" and index + 1 < len(parts):
            return parts[index + 1]
    return None


def record_windows_runtime(toolchain_path: Path, runtimes: Sequence[Path]) -> None:
    source = require_file(toolchain_path, "toolchain evidence")
    try:
        metadata = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SnapshotBuildError(f"toolchain evidence is not valid JSON: {error.msg}") from error
    if not isinstance(metadata, dict):
        raise SnapshotBuildError("toolchain evidence must be a JSON object")
    versions = sorted({value for runtime in runtimes if (value := msvc_redist_version(runtime))})
    if len(versions) > 1:
        raise SnapshotBuildError("Windows runtime closure mixes multiple MSVC redistributable versions")
    metadata["windows_runtime"] = {
        "files": [
            {"name": runtime.name, "sha256": sha256_file(runtime)}
            for runtime in sorted(runtimes, key=lambda item: item.name.lower())
        ],
        "msvc_redist_version": versions[0] if versions else "not-required",
        "redistribution_policy": MICROSOFT_REDISTRIBUTION_POLICY,
    }
    source.write_text(canonical_json(metadata), encoding="utf-8")


def package_desktop(args: argparse.Namespace) -> None:
    require_revision(args.source_commit)
    require_version(args.version)
    install_root = args.install_root.resolve()
    executable = install_root / ("mc_main.exe" if args.platform == "windows" else "mc_main")
    shaders = install_root / "shaders"
    require_file(executable, "installed executable")
    if not shaders.is_dir():
        raise SnapshotBuildError(f"installed shader directory is missing: {shaders}")
    work_root = args.work_root.resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    license_root = prepare_licenses(
        args.project_license,
        args.vcpkg_installed,
        work_root / "licenses",
        include_vendor=args.platform == "macos",
    )
    command = [
        sys.executable,
        str(require_file(args.packager, "snapshot packager")),
        "desktop",
        "--platform", args.platform,
        "--format", "zip" if args.platform == "windows" else "tar.gz",
        "--executable", str(executable),
        "--shaders", str(shaders),
        "--license", str(license_root),
        "--version", args.version,
        "--source-commit", args.source_commit,
        "--toolchain-evidence", str(require_file(args.toolchain_evidence, "toolchain evidence")),
        "--output", str(args.output),
    ]
    if args.platform == "windows":
        runtime_roots = args.runtime_search or [args.vcpkg_installed]
        runtimes = resolve_windows_runtime(executable, runtime_roots)
        record_windows_runtime(args.toolchain_evidence, runtimes)
        for runtime in runtimes:
            command.extend(("--runtime", str(runtime)))
    else:
        if args.sdk_root is None:
            raise SnapshotBuildError("macOS packaging requires --sdk-root")
        sdk_root = args.sdk_root.resolve()
        command.extend((
            "--vulkan-loader", str(sdk_root / "lib/libvulkan.1.4.357.dylib"),
            "--moltenvk", str(sdk_root / "lib/libMoltenVK.dylib"),
            "--icd-json", str(sdk_root / "share/vulkan/icd.d/MoltenVK_icd.json"),
        ))
    run(command)


def verify_android_source(root: Path) -> None:
    missing = [relative for relative in ANDROID_REQUIRED_FILES if not (root / relative).is_file()]
    if missing:
        raise SnapshotBuildError("Android build source is incomplete: " + ", ".join(missing))
    wrapper = (root / "gradle/wrapper/gradle-wrapper.properties").read_text(encoding="utf-8")
    if "gradle-8.13-bin.zip" not in wrapper or not re.search(
        r"^distributionSha256Sum=[0-9a-f]{64}$",
        wrapper,
        re.MULTILINE,
    ):
        raise SnapshotBuildError("Android Gradle wrapper must pin Gradle 8.13 and its distribution SHA-256")


def property_value(source: Path, name: str) -> str:
    for line in require_file(source, name).read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() == name:
            return value.strip()
    raise SnapshotBuildError(f"{name} is missing from {source}")


def android_sdk_root_from_environment() -> Path:
    """Return the one Android SDK root selected by the process environment."""
    values = {
        name: value.strip()
        for name in ("ANDROID_SDK_ROOT", "ANDROID_HOME")
        if (value := os.environ.get(name, "").strip())
    }
    roots = {Path(value).resolve() for value in values.values()}
    if not roots:
        raise SnapshotBuildError("ANDROID_SDK_ROOT or ANDROID_HOME is required")
    if len(roots) != 1:
        details = ", ".join(f"{name}={values[name]}" for name in sorted(values))
        raise SnapshotBuildError(f"Android SDK environment variables must agree: {details}")
    return roots.pop()


def record_android_metadata(args: argparse.Namespace) -> None:
    require_revision(args.source_commit)
    root = args.android_root.resolve()
    verify_android_source(root)
    sdk_root = android_sdk_root_from_environment()
    platform_properties = sdk_root / f"platforms/android-{args.api_level}/source.properties"
    ndk_properties = sdk_root / f"ndk/{args.ndk_version}/source.properties"
    cmake_properties = sdk_root / f"cmake/{args.cmake_version}/source.properties"
    java = run(["java", "-version"])
    if not re.search(r'(?:version |openjdk )["\']?21(?:[.\s"\'])', java, re.IGNORECASE):
        raise SnapshotBuildError(f"Java 21 is required, got: {java}")
    gradle = run([str(root / "gradlew"), "--no-daemon", "--version"], cwd=root)
    if "Gradle 8.13" not in gradle:
        raise SnapshotBuildError(f"Gradle mismatch: expected 8.13, command reported: {gradle}")
    metadata = {
        "schema": 1,
        "revision": args.source_commit,
        "platform": "android",
        "configuration": "debug",
        "abi": "arm64-v8a",
        "api_level": args.api_level,
        "java": java,
        "gradle": gradle,
        "android_platform": property_value(platform_properties, "Pkg.Revision"),
        "android_ndk": property_value(ndk_properties, "Pkg.Revision"),
        "android_cmake": property_value(cmake_properties, "Pkg.Revision"),
        "vcpkg_revision": run(["git", "-C", str(args.vcpkg_root), "rev-parse", "HEAD"]),
    }
    if metadata["android_ndk"] != args.ndk_version:
        raise SnapshotBuildError(f"Android NDK mismatch: expected {args.ndk_version}, got {metadata['android_ndk']}")
    if metadata["android_cmake"] != args.cmake_version:
        raise SnapshotBuildError(f"Android CMake mismatch: expected {args.cmake_version}, got {metadata['android_cmake']}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(canonical_json(metadata), encoding="utf-8")


def write_release_evidence(args: argparse.Namespace) -> None:
    revision = require_revision(args.source_commit)
    version = require_version(args.version)
    artifact = require_file(args.artifact, "release artifact")
    toolchain_path = require_file(args.toolchain_evidence, "toolchain evidence")
    try:
        toolchain = json.loads(toolchain_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SnapshotBuildError(f"toolchain evidence is not valid JSON: {error.msg}") from error
    if not isinstance(toolchain, dict) or toolchain.get("revision") != revision:
        raise SnapshotBuildError("toolchain evidence must be an object for the exact source commit")
    runtime_requirements = {
        "macos": "Run the extracted exact archive locally through MinecraftClone.command before release.",
        "windows": "Windows runtime acceptance must be supplied by the user; hosted build evidence is not runtime evidence.",
        "android": "Install and launch the exact development-signed APK locally before release.",
    }
    evidence = {
        "schema": 1,
        "platform": args.platform,
        "version": version,
        "source_commit": revision,
        "artifact": {
            "name": artifact.name,
            "bytes": artifact.stat().st_size,
            "sha256": sha256_file(artifact),
        },
        "build": {
            "configuration": args.configuration,
            "status": "passed",
            "toolchain": toolchain,
            "toolchain_evidence_sha256": sha256_file(toolchain_path),
        },
        "automated_verification": args.automated_verification,
        "runtime": {"status": "not-run", "requirement": runtime_requirements[args.platform]},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(canonical_json(evidence), encoding="utf-8")


def write_checksum(artifact: Path, output: Path) -> None:
    artifact = require_file(artifact, "release artifact")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(f"{sha256_file(artifact)}  {artifact.name}\n", encoding="utf-8")


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)

    source = commands.add_parser("source-identity")
    source.add_argument("--root", type=Path, default=Path.cwd())
    source.add_argument("--expected")
    source.add_argument("--requested-ref")
    source.add_argument("--version", default="")
    source.add_argument("--output", type=Path, required=True)
    source.add_argument("--github-output", type=Path)

    desktop = commands.add_parser("package-desktop")
    desktop.add_argument("--platform", choices=("macos", "windows"), required=True)
    desktop.add_argument("--install-root", type=Path, required=True)
    desktop.add_argument("--vcpkg-installed", type=Path, required=True)
    desktop.add_argument("--runtime-search", type=Path, action="append", default=[])
    desktop.add_argument("--sdk-root", type=Path)
    desktop.add_argument("--project-license", type=Path, default=Path("LICENSE"))
    desktop.add_argument("--packager", type=Path, default=Path("script/package_snapshot.py"))
    desktop.add_argument("--toolchain-evidence", type=Path, required=True)
    desktop.add_argument("--work-root", type=Path, required=True)
    desktop.add_argument("--version", required=True)
    desktop.add_argument("--source-commit", required=True)
    desktop.add_argument("--output", type=Path, required=True)

    licenses = commands.add_parser("license-archive")
    licenses.add_argument("--project-license", type=Path, default=Path("LICENSE"))
    licenses.add_argument("--vcpkg-installed", type=Path, required=True)
    licenses.add_argument("--work-root", type=Path, required=True)
    licenses.add_argument("--output", type=Path, required=True)

    android_source = commands.add_parser("verify-android-source")
    android_source.add_argument("--root", type=Path, default=Path("android"))

    android_metadata = commands.add_parser("record-android-metadata")
    android_metadata.add_argument("--android-root", type=Path, default=Path("android"))
    android_metadata.add_argument("--api-level", type=int, default=35)
    android_metadata.add_argument("--ndk-version", default="27.0.12077973")
    android_metadata.add_argument("--cmake-version", default="3.30.5")
    android_metadata.add_argument("--vcpkg-root", type=Path, required=True)
    android_metadata.add_argument("--source-commit", required=True)
    android_metadata.add_argument("--output", type=Path, required=True)

    evidence = commands.add_parser("write-evidence")
    evidence.add_argument("--platform", choices=("macos", "windows", "android"), required=True)
    evidence.add_argument("--configuration", required=True)
    evidence.add_argument("--automated-verification", choices=("passed", "build-only"), required=True)
    evidence.add_argument("--version", required=True)
    evidence.add_argument("--source-commit", required=True)
    evidence.add_argument("--artifact", type=Path, required=True)
    evidence.add_argument("--toolchain-evidence", type=Path, required=True)
    evidence.add_argument("--output", type=Path, required=True)

    checksum = commands.add_parser("write-checksum")
    checksum.add_argument("--artifact", type=Path, required=True)
    checksum.add_argument("--output", type=Path, required=True)
    return root


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "source-identity":
            write_source_identity(args.root.resolve(), args.expected, args.requested_ref, args.version, args.output, args.github_output)
        elif args.command == "package-desktop":
            package_desktop(args)
        elif args.command == "license-archive":
            licenses = prepare_licenses(args.project_license, args.vcpkg_installed, args.work_root / "licenses", False)
            write_license_archive(licenses, args.output)
        elif args.command == "verify-android-source":
            verify_android_source(args.root.resolve())
        elif args.command == "record-android-metadata":
            record_android_metadata(args)
        elif args.command == "write-evidence":
            write_release_evidence(args)
        else:
            write_checksum(args.artifact, args.output)
    except (OSError, SnapshotBuildError) as error:
        print(f"snapshot build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
