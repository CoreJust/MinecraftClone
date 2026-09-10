#!/usr/bin/env python3
"""Acquire and verify the exact CI toolchain without action-side installers."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform as host_platform
import shutil
import stat
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path
from typing import Sequence


CMAKE_VERSION = "3.31.6"
NINJA_VERSION = "1.13.1"
PYTHON_VERSION = "3.12.10"
VCPKG_COMMIT = "2b65c20fc66eda893aa15a15a453c3cf09500b19"
VULKAN_VERSION = "1.4.357.0"
VULKAN_DOWNLOADS = {
    "windows": {
        "url": f"https://sdk.lunarg.com/sdk/download/{VULKAN_VERSION}/windows/vulkan_sdk.exe",
        "sha256": "81f474711e9042f4cd22b31b2f7a8870db2e428b21586fb43dd80150be97310d",
        "filename": "vulkan_sdk.exe",
    },
    "macos": {
        "url": f"https://sdk.lunarg.com/sdk/download/{VULKAN_VERSION}/mac/vulkan_sdk.zip",
        "sha256": "539433589c83522e6f31b1c7b418a4167e21597a4a361ab119e1dc0760cf3865",
        "filename": "vulkan_sdk.zip",
    },
}
ANDROID_COMMAND_LINE_TOOLS = {
    "url": "https://dl.google.com/android/repository/commandlinetools-linux-15859902_latest.zip",
    "sha256": "4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583",
    "filename": "commandlinetools-linux-15859902_latest.zip",
}
ANDROID_SDK_PACKAGES = (
    "platforms;android-35",
    "ndk;27.0.12077973",
    "cmake;3.30.5",
)
DOWNLOAD_USER_AGENT = "MinecraftClone CI acquisition"
NINJA_DOWNLOADS = {
    "windows": {
        "url": f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-win.zip",
        "sha256": "26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf",
        "filename": "ninja-win.zip",
    },
    "macos": {
        "url": f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-mac.zip",
        "sha256": "da7797794153629aca5570ef7c813342d0be214ba84632af886856e8f0063dd9",
        "filename": "ninja-mac.zip",
    },
}
MESH_SHADERS = ("grid.mesh.spv", "player.mesh.spv")
VULKAN_12_SHADERS = ("grid.vert.spv", "player.vert.spv", "trivial.frag.spv")


class CiError(RuntimeError):
    """A pinned dependency or required CI tool was unavailable or mismatched."""


def run(command: Sequence[str], accepted: Sequence[int] = (0,), input_text: str | None = None) -> str:
    """Run a command and return combined output, accepting only stated statuses."""
    try:
        completed = subprocess.run(
            command,
            text=True,
            input=input_text,
            capture_output=True,
            check=False,
        )
    except OSError as error:
        raise CiError(f"could not start {' '.join(command)}: {error}") from error
    output = completed.stdout + completed.stderr
    if completed.returncode not in accepted:
        raise CiError(f"{' '.join(command)} exited {completed.returncode}: {output.strip()}")
    return output.strip()


def sha256_file(path: Path) -> str:
    """Return the SHA-256 digest for an already-downloaded artifact."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_sha256(path: Path, expected: str) -> None:
    """Reject a download whose full content hash is not the pinned digest."""
    actual = sha256_file(path)
    if actual.lower() != expected.lower():
        raise CiError(f"SHA-256 mismatch for {path}: expected {expected}, got {actual}")


def download(url: str, destination: Path) -> None:
    """Download an artifact into its destination without using a mutable action."""
    try:
        request = urllib.request.Request(url, headers={"User-Agent": DOWNLOAD_USER_AGENT})
        with urllib.request.urlopen(request) as response, destination.open("wb") as stream:
            shutil.copyfileobj(response, stream)
    except OSError as error:
        raise CiError(f"could not download {url}: {error}") from error


def write_github_env(name: str, value: str) -> None:
    """Persist one variable for subsequent GitHub Actions steps when requested."""
    destination = os.environ.get("GITHUB_ENV")
    if destination:
        with Path(destination).open("a", encoding="utf-8") as stream:
            stream.write(f"{name}={value}\n")


def write_github_path(value: Path) -> None:
    """Expose a pinned SDK executable directory to later GitHub Actions steps."""
    destination = os.environ.get("GITHUB_PATH")
    if destination:
        with Path(destination).open("a", encoding="utf-8") as stream:
            stream.write(f"{value}\n")


def write_android_sdk_environment(root: Path) -> None:
    """Export both Android SDK variable names to the same acquired root."""
    value = str(root)
    write_github_env("ANDROID_HOME", value)
    write_github_env("ANDROID_SDK_ROOT", value)


def safe_extract(archive: Path, destination: Path) -> None:
    """Extract a hash-verified archive without allowing paths outside destination."""
    root = destination.resolve()
    with zipfile.ZipFile(archive) as contents:
        for member in contents.infolist():
            target = (destination / member.filename).resolve()
            if target != root and root not in target.parents:
                raise CiError(f"archive entry escapes destination: {member.filename}")
        contents.extractall(destination)


def find_ninja(root: Path, platform_name: str) -> Path:
    """Find the single executable in the pinned platform-specific Ninja archive."""
    name = "ninja.exe" if platform_name == "windows" else "ninja"
    candidates = sorted(path for path in root.rglob(name) if path.is_file())
    if len(candidates) != 1:
        raise CiError(f"expected one {name} below {root}, found {len(candidates)}")
    return candidates[0]


def install_ninja(platform_name: str, root: Path) -> Path:
    """Install a hash-verified upstream Ninja release instead of a mutable package."""
    config = NINJA_DOWNLOADS[platform_name]
    root.mkdir(parents=True, exist_ok=True)
    archive = root.parent / config["filename"]
    download(config["url"], archive)
    verify_sha256(archive, config["sha256"])
    safe_extract(archive, root)
    executable = find_ninja(root, platform_name)
    if platform_name == "macos":
        executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
    write_github_path(executable.parent)
    print(executable)
    return executable


def find_vulkan_sdk(root: Path) -> tuple[Path, Path]:
    """Find the single SDK root and glslc binary in an extracted SDK payload."""
    candidates = sorted(
        path for path in root.rglob("glslc*")
        if path.is_file() and path.name in {"glslc", "glslc.exe"}
    )
    if len(candidates) != 1:
        raise CiError(f"expected one glslc below {root}, found {len(candidates)}")
    compiler = candidates[0]
    return compiler.parent.parent, compiler


def find_macos_vulkan_installer(root: Path) -> Path:
    """Locate the SDK installer executable packaged inside the macOS zip."""
    name = f"vulkansdk-macOS-{VULKAN_VERSION}"
    candidates = sorted(
        path for path in root.rglob(name)
        if path.is_file() and path.parent.name == "MacOS" and path.parent.parent.name == "Contents"
    )
    if len(candidates) != 1:
        raise CiError(f"expected one macOS Vulkan SDK installer below {root}, found {len(candidates)}")
    return candidates[0]


def install_vulkan(platform_name: str, root: Path) -> Path:
    """Install the pinned Vulkan SDK and export its exact discovered location."""
    config = VULKAN_DOWNLOADS[platform_name]
    root.mkdir(parents=True, exist_ok=True)
    archive = root.parent / config["filename"]
    download(f"{config['url']}?Human=true", archive)
    verify_sha256(archive, config["sha256"])
    if platform_name == "windows":
        run([
            str(archive),
            "--root", str(root),
            "--accept-licenses",
            "--default-answer",
            "--confirm-command", "install",
            "copy_only=1",
        ])
    else:
        extracted_root = root / "installer"
        installed_root = root / "installed"
        extracted_root.mkdir()
        safe_extract(archive, extracted_root)
        installer = find_macos_vulkan_installer(extracted_root)
        installer.chmod(installer.stat().st_mode | stat.S_IXUSR)
        run([
            str(installer),
            "--root", str(installed_root),
            "--accept-licenses",
            "--default-answer",
            "--confirm-command", "install",
            "copy_only=1",
        ])
        root = installed_root
    sdk_root, compiler = find_vulkan_sdk(root)
    write_github_env("VULKAN_SDK", str(sdk_root))
    write_github_path(compiler.parent)
    print(sdk_root)
    return sdk_root


def install_android_sdk(root: Path) -> Path:
    """Bootstrap pinned command-line tools before installing exact Android SDK packages."""
    root.mkdir(parents=True, exist_ok=True)
    latest = root / "cmdline-tools" / "latest"
    if latest.exists():
        raise CiError(f"Android command-line tools already exist: {latest}")
    archive = root.parent / ANDROID_COMMAND_LINE_TOOLS["filename"]
    download(ANDROID_COMMAND_LINE_TOOLS["url"], archive)
    verify_sha256(archive, ANDROID_COMMAND_LINE_TOOLS["sha256"])
    staging = root / "command-line-tools"
    staging.mkdir()
    safe_extract(archive, staging)
    extracted_tools = staging / "cmdline-tools"
    if not extracted_tools.is_dir():
        raise CiError(f"Android command-line tools archive is missing {extracted_tools}")
    latest.parent.mkdir(parents=True)
    extracted_tools.rename(latest)
    staging.rmdir()
    sdkmanager = latest / "bin" / "sdkmanager"
    if not sdkmanager.is_file():
        raise CiError(f"Android command-line tools archive is missing {sdkmanager}")
    sdkmanager.chmod(sdkmanager.stat().st_mode | stat.S_IXUSR)
    command = [str(sdkmanager), f"--sdk_root={root}"]
    run([*command, "--licenses"], input_text="y\n" * 100)
    run([*command, *ANDROID_SDK_PACKAGES])
    write_android_sdk_environment(root)
    print(root)
    return sdkmanager


def install_vcpkg(root: Path) -> None:
    """Clone, detach at, bootstrap, and expose the exact vcpkg revision."""
    if root.exists():
        raise CiError(f"vcpkg root already exists: {root}")
    run(["git", "clone", "--filter=blob:none", "https://github.com/microsoft/vcpkg.git", str(root)])
    run(["git", "-C", str(root), "checkout", "--detach", VCPKG_COMMIT])
    actual = run(["git", "-C", str(root), "rev-parse", "HEAD"])
    if actual != VCPKG_COMMIT:
        raise CiError(f"vcpkg revision mismatch: expected {VCPKG_COMMIT}, got {actual}")
    if os.name == "nt":
        run(["cmd.exe", "/d", "/c", "call", str(root / "bootstrap-vcpkg.bat"), "-disableMetrics"])
    else:
        run([str(root / "bootstrap-vcpkg.sh"), "-disableMetrics"])
    write_github_env("VCPKG_ROOT", str(root))
    print(root)


def require_prefix(label: str, output: str, expected: str) -> None:
    """Require a command's recorded version output to include its exact pin."""
    if expected not in output:
        raise CiError(f"{label} does not report required {expected}: {output}")


def expected_sdk_include() -> Path:
    """Return the Windows SDK include location asserted by the runner contract."""
    program_files = os.environ.get("ProgramFiles(x86)")
    if not program_files:
        raise CiError("ProgramFiles(x86) is unavailable; cannot verify Windows SDK")
    return Path(program_files) / "Windows Kits" / "10" / "Include" / "10.0.26100.0"


def verify_tools(platform_name: str) -> None:
    """Fail before configuring if the selected hosted image differs from the contract."""
    require_prefix("CMake", run(["cmake", "--version"]), CMAKE_VERSION)
    require_prefix("Ninja", run(["ninja", "--version"]), NINJA_VERSION)
    python_version = ".".join(str(value) for value in sys.version_info[:3])
    if python_version != PYTHON_VERSION:
        raise CiError(f"Python mismatch: expected {PYTHON_VERSION}, got {python_version}")
    glslc = shutil.which("glslc")
    if not glslc:
        raise CiError("glslc is missing from the installed Vulkan SDK")
    run([glslc, "--version"])
    if platform_name == "macos":
        if sys.platform != "darwin" or host_platform.machine() != "arm64":
            raise CiError(f"expected macOS arm64 runner, got {sys.platform} {host_platform.machine()}")
        require_prefix("Xcode", run(["xcodebuild", "-version"]), "Xcode 26.2")
        return
    if platform_name == "windows":
        if os.name != "nt" or host_platform.machine().lower() not in {"amd64", "x86_64"}:
            raise CiError(f"expected Windows AMD64 runner, got {os.name} {host_platform.machine()}")
        require_prefix("MSVC", run(["cl"], accepted=(0, 2)), "Version 19.44.")
        if not expected_sdk_include().is_dir():
            raise CiError(f"Windows SDK 10.0.26100.0 is missing: {expected_sdk_include()}")
        return
    raise CiError(f"unsupported platform: {platform_name}")


def command_version(command: Sequence[str], accepted: Sequence[int] = (0,)) -> str:
    """Record a missing tool as an error rather than silently omitting metadata."""
    return run(command, accepted=accepted)


def record_metadata(platform_name: str, preset: str, output: Path) -> None:
    """Write exact revision and actual tool outputs for the diagnostic artifact."""
    metadata: dict[str, object] = {
        "revision": command_version(["git", "rev-parse", "HEAD"]),
        "platform": platform_name,
        "preset": preset,
        "architecture": host_platform.machine(),
        "python": sys.version,
        "cmake": command_version(["cmake", "--version"]),
        "ninja": command_version(["ninja", "--version"]),
        "glslc": command_version(["glslc", "--version"]),
        "vulkan_sdk": os.environ.get("VULKAN_SDK", "missing"),
    }
    vcpkg_root = os.environ.get("VCPKG_ROOT")
    if not vcpkg_root:
        raise CiError("VCPKG_ROOT is missing")
    metadata["vcpkg_revision"] = command_version(["git", "-C", vcpkg_root, "rev-parse", "HEAD"])
    if platform_name == "macos":
        metadata["xcode"] = command_version(["xcodebuild", "-version"])
    elif platform_name == "windows":
        metadata["msvc"] = command_version(["cl"], accepted=(0, 2))
        metadata["windows_sdk_include"] = str(expected_sdk_include())
    else:
        raise CiError(f"unsupported platform: {platform_name}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def shader_path(build_dir: Path, shader: str) -> Path:
    """Resolve a generated shader without validating copied runtime duplicates twice."""
    path = build_dir / "src" / "client" / shader
    if not path.is_file():
        raise CiError(f"generated shader is missing: {path}")
    return path


def spirv_validator() -> str:
    """Resolve spirv-val from PATH or the just-installed pinned SDK."""
    validator = shutil.which("spirv-val")
    if validator:
        return validator
    sdk_root = os.environ.get("VULKAN_SDK")
    if sdk_root:
        for directory in (Path(sdk_root) / "bin", Path(sdk_root) / "Bin"):
            candidate = directory / ("spirv-val.exe" if os.name == "nt" else "spirv-val")
            if candidate.is_file():
                return str(candidate)
    raise CiError("spirv-val is missing from the installed Vulkan SDK")


def validate_shaders(build_dir: Path) -> None:
    """Validate mesh shaders against Vulkan 1.3 and fallback stages against 1.2."""
    validator = spirv_validator()
    for shader in MESH_SHADERS:
        run([validator, "--target-env", "vulkan1.3", str(shader_path(build_dir, shader))])
    for shader in VULKAN_12_SHADERS:
        run([validator, "--target-env", "vulkan1.2", str(shader_path(build_dir, shader))])


def source_checks(pre_finalization_candidate: bool = False) -> None:
    """Run repository policy/source checks without a duplicate application build."""
    root = Path(__file__).resolve().parents[2]
    run([sys.executable, "script/ai_check.py", "--fast"])
    sys.path.insert(0, str(root))
    from script.ai_check import project_version_arguments

    names, version = project_version_arguments(root)
    command = [sys.executable, "publish.py", names, version, "--checks-only"]
    if pre_finalization_candidate:
        command.append("--pre-finalization-candidate")
    run(command)


def main(argv: Sequence[str] | None = None) -> int:
    """Dispatch a single deterministic CI acquisition or validation operation."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    install_vulkan_parser = commands.add_parser("install-vulkan")
    install_vulkan_parser.add_argument("--platform", choices=tuple(VULKAN_DOWNLOADS), required=True)
    install_vulkan_parser.add_argument("--root", type=Path, required=True)
    install_ninja_parser = commands.add_parser("install-ninja")
    install_ninja_parser.add_argument("--platform", choices=tuple(NINJA_DOWNLOADS), required=True)
    install_ninja_parser.add_argument("--root", type=Path, required=True)
    install_vcpkg_parser = commands.add_parser("install-vcpkg")
    install_vcpkg_parser.add_argument("--root", type=Path, required=True)
    install_android_parser = commands.add_parser("install-android-sdk")
    install_android_parser.add_argument("--root", type=Path, required=True)
    verify_parser = commands.add_parser("verify-tools")
    verify_parser.add_argument("--platform", choices=tuple(VULKAN_DOWNLOADS), required=True)
    metadata_parser = commands.add_parser("record-metadata")
    metadata_parser.add_argument("--platform", choices=tuple(VULKAN_DOWNLOADS), required=True)
    metadata_parser.add_argument("--preset", choices=("debug", "release"), required=True)
    metadata_parser.add_argument("--output", type=Path, required=True)
    shader_parser = commands.add_parser("validate-shaders")
    shader_parser.add_argument("--build-dir", type=Path, required=True)
    source_checks_parser = commands.add_parser("source-checks")
    source_checks_parser.add_argument("--pre-finalization-candidate", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.command == "install-vulkan":
            install_vulkan(args.platform, args.root)
        elif args.command == "install-ninja":
            install_ninja(args.platform, args.root)
        elif args.command == "install-vcpkg":
            install_vcpkg(args.root)
        elif args.command == "install-android-sdk":
            install_android_sdk(args.root)
        elif args.command == "verify-tools":
            verify_tools(args.platform)
        elif args.command == "record-metadata":
            record_metadata(args.platform, args.preset, args.output)
        elif args.command == "validate-shaders":
            validate_shaders(args.build_dir)
        else:
            source_checks(args.pre_finalization_candidate)
    except CiError as error:
        print(f"CI setup failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
