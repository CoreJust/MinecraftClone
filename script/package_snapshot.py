#!/usr/bin/env python3
"""Create reproducible desktop package archives or record Android APK evidence.

This tool deliberately does not build, sign, upload, or launch anything.  Its
inputs are already-built artifacts, so a release process can bind its package
to an exact source revision and toolchain record without silently reading a
developer's SDK installation.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO, Iterable, Sequence


REVISION_RE = re.compile(r"[0-9a-f]{40}")
VERSION_RE = re.compile(r"\d+\.\d+\.\d+(?::\d+)?")
MACOS_SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")
WINDOWS_RESERVED_NAMES = {
    "CON", "PRN", "AUX", "NUL",
    *(f"COM{number}" for number in range(1, 10)),
    *(f"LPT{number}" for number in range(1, 10)),
    "COM¹", "COM²", "COM³", "LPT¹", "LPT²", "LPT³",
}
WINDOWS_INVALID_CHARACTERS = frozenset('<>:"\\|?*')
ANDROID_NATIVE_LIBRARY = "libmc_android.so"
ANDROID_ELF_ABIS = {
    (1, 3): "x86",
    (1, 40): "armeabi-v7a",
    (2, 62): "x86_64",
    (2, 183): "arm64-v8a",
}


class PackageError(ValueError):
    """Raised when an input cannot safely produce the requested artifact."""


def android_elf_abi(header: bytes, entry_name: str) -> str:
    if len(header) < 20 or header[:4] != b"\x7fELF" or header[5:7] != b"\x01\x01":
        raise PackageError(f"APK native library has a malformed ELF header: {entry_name}")
    elf_class = header[4]
    header_size = {1: 52, 2: 64}.get(elf_class)
    if header_size is None or len(header) < header_size:
        raise PackageError(f"APK native library has a malformed ELF header: {entry_name}")
    size_offset = 40 if elf_class == 1 else 52
    if int.from_bytes(header[size_offset:size_offset + 2], "little") != header_size:
        raise PackageError(f"APK native library has a malformed ELF header: {entry_name}")
    machine = int.from_bytes(header[18:20], "little")
    abi = ANDROID_ELF_ABIS.get((elf_class, machine))
    if abi is None:
        raise PackageError(f"APK native library has an unsupported ELF architecture: {entry_name}")
    return abi


@dataclass(frozen=True)
class ArchiveEntry:
    name: str
    data: bytes
    mode: int

    def bytes(self) -> bytes:
        return self.data


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def strict_json(data: bytes, label: str) -> Any:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise PackageError(f"{label} is not UTF-8") from error

    def reject_constant(value: str) -> None:
        raise PackageError(f"{label} contains non-standard JSON constant: {value}")

    try:
        return json.loads(text, parse_constant=reject_constant)
    except json.JSONDecodeError as error:
        raise PackageError(f"{label} is not JSON: {error.msg}") from error


def require_regular_file(source: Path, label: str) -> Path:
    if source.is_symlink() or not source.exists() or not source.is_file():
        raise PackageError(f"{label} must be an existing regular file: {source}")
    return source.resolve()


def snapshot_regular_file(source: Path, label: str) -> tuple[Path, bytes]:
    candidate = source.absolute()
    try:
        before = candidate.lstat()
        if not stat.S_ISREG(before.st_mode):
            raise PackageError(f"{label} must be an existing regular file: {source}")
        with candidate.open("rb") as stream:
            opened = os.fstat(stream.fileno())
            if not stat.S_ISREG(opened.st_mode):
                raise PackageError(f"{label} must be an existing regular file: {source}")
            data = stream.read()
            after = os.fstat(stream.fileno())
        current = candidate.lstat()
    except OSError as error:
        raise PackageError(f"{label} must be an existing regular file: {source}") from error
    identity = lambda value: (value.st_dev, value.st_ino)
    fingerprint = lambda value: (value.st_size, value.st_mtime_ns)
    if (
        identity(before) != identity(opened)
        or identity(opened) != identity(after)
        or identity(after) != identity(current)
        or fingerprint(before) != fingerprint(after)
        or fingerprint(after) != fingerprint(current)
    ):
        raise PackageError(f"{label} changed while it was being read: {source}")
    return candidate, data


def require_directory(source: Path, label: str) -> Path:
    if source.is_symlink() or not source.exists() or not source.is_dir():
        raise PackageError(f"{label} must be an existing directory: {source}")
    return source.resolve()


def archive_name(relative: str) -> str:
    candidate = PurePosixPath(relative)
    if not relative or candidate.is_absolute() or ".." in candidate.parts or "." in candidate.parts:
        raise PackageError(f"unsafe archive path: {relative}")
    return candidate.as_posix()


def files_in_tree(source: Path, destination: str, label: str) -> list[ArchiveEntry]:
    root = require_directory(source, label)
    entries: list[ArchiveEntry] = []
    for child in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        if child.is_symlink():
            raise PackageError(f"{label} contains a symlink: {child}")
        if child.is_dir():
            continue
        if not child.is_file():
            raise PackageError(f"{label} contains a non-regular file: {child}")
        relative = child.relative_to(root).as_posix()
        _, data = snapshot_regular_file(child, label)
        entries.append(ArchiveEntry(archive_name(f"{destination}/{relative}"), data, 0o644))
    if not entries:
        raise PackageError(f"{label} must not be empty: {source}")
    return entries


def add_file(entries: list[ArchiveEntry], source: Path, destination: str, label: str, mode: int = 0o644) -> None:
    _, data = snapshot_regular_file(source, label)
    entries.append(ArchiveEntry(archive_name(destination), data, mode))


def reject_duplicate_names(entries: Iterable[ArchiveEntry]) -> None:
    names = [entry.name for entry in entries]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise PackageError("duplicate archive paths: " + ", ".join(duplicates))


def validate_windows_archive_names(entries: Iterable[ArchiveEntry]) -> None:
    names = [entry.name for entry in entries]
    folded = [name.casefold() for name in names]
    collisions = sorted({names[index] for index, name in enumerate(folded) if folded.count(name) > 1})
    if collisions:
        raise PackageError("case-insensitive Windows archive path collision: " + ", ".join(collisions))
    for name in names:
        for part in PurePosixPath(name).parts:
            stem = part.split(".", 1)[0].upper()
            if (
                part.endswith((".", " "))
                or stem in WINDOWS_RESERVED_NAMES
                or any(ord(character) < 32 or character in WINDOWS_INVALID_CHARACTERS for character in part)
            ):
                raise PackageError(f"invalid Windows archive path: {name}")


def toolchain_evidence(source: Path) -> dict[str, Any]:
    _, data = snapshot_regular_file(source, "toolchain evidence")
    value = strict_json(data, f"toolchain evidence {source}")
    if not isinstance(value, dict):
        raise PackageError("toolchain evidence must be a JSON object")
    return value


def verify_common(args: argparse.Namespace) -> dict[str, Any]:
    if not REVISION_RE.fullmatch(args.source_commit):
        raise PackageError("--source-commit must be a 40-character lowercase hexadecimal revision")
    if not VERSION_RE.fullmatch(args.version):
        raise PackageError("--version must use MAJOR.MINOR.PATCH[:SNAPSHOT]")
    return toolchain_evidence(args.toolchain_evidence)


def macos_architectures(binary: Path) -> frozenset[str]:
    completed = subprocess.run(["lipo", "-archs", str(binary)], text=True, capture_output=True, check=False)
    if completed.returncode:
        raise PackageError(f"lipo could not inspect {binary}: {completed.stderr.strip()}")
    output = completed.stdout.strip()
    if " are: " in output:
        names = output.rsplit(" are: ", 1)[1].split()
    elif " architecture: " in output:
        names = [output.rsplit(" architecture: ", 1)[1].strip()]
    else:
        names = output.split()
    if not names or len(names) != len(set(names)):
        raise PackageError(f"unexpected lipo output for {binary}: {output}")
    result = frozenset(names)
    if not result or not result <= {"arm64", "x86_64"}:
        raise PackageError(f"unsupported macOS architecture set for {binary}: {' '.join(sorted(result))}")
    return result


def macos_dependencies(binary: Path) -> list[str]:
    completed = subprocess.run(["otool", "-L", str(binary)], text=True, capture_output=True, check=False)
    if completed.returncode:
        raise PackageError(f"otool could not inspect {binary}: {completed.stderr.strip()}")
    dependencies = []
    for line in completed.stdout.splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        dependencies.append(line.split(" (", 1)[0])
    if not dependencies:
        raise PackageError(f"otool reported no dependencies for {binary}")
    return dependencies


def resolve_macos_dependency(
    dependency: str,
    binary_location: PurePosixPath,
) -> PurePosixPath | None:
    for prefix, base in (
        ("@loader_path/", binary_location.parent),
        ("@executable_path/", PurePosixPath()),
    ):
        if dependency.startswith(prefix):
            parts = list(base.parts)
            for part in PurePosixPath(dependency.removeprefix(prefix)).parts:
                if part in {"", "."}:
                    continue
                if part == "..":
                    if not parts:
                        return None
                    parts.pop()
                else:
                    parts.append(part)
            return PurePosixPath(*parts)
    if dependency.startswith("@rpath/"):
        relative = PurePosixPath(dependency.removeprefix("@rpath/"))
        if len(relative.parts) != 1:
            return None
        return PurePosixPath("lib") / relative
    return None


def validate_macos_package(
    binaries: Sequence[tuple[Path, PurePosixPath]],
) -> list[tuple[Path, PurePosixPath, bytes]]:
    snapshots: list[tuple[Path, PurePosixPath, bytes]] = []
    for binary, location in binaries:
        source, data = snapshot_regular_file(binary, "macOS binary")
        snapshots.append((source, location, data))
    packaged_locations = frozenset(location for _, location in binaries[1:])
    with tempfile.TemporaryDirectory() as directory:
        inspection_paths = []
        for index, (source, _, data) in enumerate(snapshots):
            inspection = Path(directory) / f"{index}{source.suffix}"
            inspection.write_bytes(data)
            inspection_paths.append(inspection)
        expected = macos_architectures(inspection_paths[0])
        for (source, binary_location, _), inspection in zip(snapshots, inspection_paths, strict=True):
            available = macos_architectures(inspection)
            if not expected <= available:
                missing = " ".join(sorted(expected - available))
                raise PackageError(
                    f"macOS binary is missing executable architecture {missing}: {source}"
                )
            for dependency in macos_dependencies(inspection):
                if dependency.startswith(("@rpath/", "@loader_path/", "@executable_path/")):
                    location = resolve_macos_dependency(dependency, binary_location)
                    if location not in packaged_locations:
                        raise PackageError(
                            f"macOS binary dependency does not resolve to a packaged file: "
                            f"{source}: {dependency}"
                        )
                    continue
                if dependency.startswith(MACOS_SYSTEM_PREFIXES):
                    continue
                raise PackageError(
                    f"macOS binary references a non-relocatable dependency: {source}: {dependency}"
                )
    return snapshots


def sanitized_icd(source: Path) -> bytes:
    _, data = snapshot_regular_file(source, "MoltenVK ICD JSON")
    value = strict_json(data, "MoltenVK ICD JSON")
    if not isinstance(value, dict) or not isinstance(value.get("ICD"), dict):
        raise PackageError("MoltenVK ICD JSON must contain an ICD object")
    value["ICD"]["library_path"] = "../../lib/libMoltenVK.dylib"
    serialized = canonical_json(value)
    if re.search(rb'"(?:[^"\\]|\\.)*"\s*:\s*"(?:/|[A-Za-z]:|~)', serialized):
        raise PackageError("MoltenVK ICD JSON contains an external machine path")
    return serialized


def macos_launcher() -> bytes:
    return b"""#!/bin/sh
set -eu
bundle_root=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)
export DYLD_LIBRARY_PATH=\"$bundle_root/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}\"
export VK_DRIVER_FILES=\"$bundle_root/vulkan/icd.d/MoltenVK_icd.json\"
exec \"$bundle_root/mc_main\" \"$@\"
"""


def desktop_entries(args: argparse.Namespace, evidence: dict[str, Any]) -> tuple[list[ArchiveEntry], dict[str, Any]]:
    executable = require_regular_file(args.executable, "executable")
    shader_entries = files_in_tree(args.shaders, "shaders", "shader directory")
    entries = list(shader_entries)
    executable_name = "mc_main" if args.platform == "macos" else "mc_main.exe"
    if args.platform == "windows" and executable.suffix.lower() != ".exe":
        raise PackageError("Windows executable must have a .exe suffix")

    license_names = []
    for source in args.license:
        candidate = Path(source)
        if candidate.is_dir() and not candidate.is_symlink():
            copied = files_in_tree(candidate, f"licenses/{candidate.name}", "license directory")
            entries.extend(copied)
            license_names.extend(entry.name for entry in copied)
        else:
            destination = f"licenses/{candidate.name}"
            add_file(entries, candidate, destination, "license")
            license_names.append(destination)
    if not license_names:
        raise PackageError("at least one --license input is required")

    runtime = [require_regular_file(Path(item), "runtime dependency") for item in args.runtime]
    if args.platform == "windows":
        if not runtime:
            raise PackageError("Windows packages require explicit --runtime DLL inputs")
        add_file(entries, executable, executable_name, "executable", 0o755)
        for item in runtime:
            if item.suffix.lower() != ".dll":
                raise PackageError(f"Windows runtime dependency must be a DLL: {item}")
            add_file(entries, item, item.name, "runtime dependency", 0o755)
        instructions = (
            "Run mc_main.exe from this directory. Vulkan requires a compatible GPU driver; "
            "the driver is not redistributed in this package.\n"
        ).encode("utf-8")
        entries.append(ArchiveEntry("LAUNCH.txt", instructions, 0o644))
        runtime_config: dict[str, Any] = {"launcher": "mc_main.exe", "vulkan_driver": "external-required"}
    else:
        if args.vulkan_loader is None or args.moltenvk is None or args.icd_json is None:
            raise PackageError("macOS packages require --vulkan-loader, --moltenvk, and --icd-json")
        loader = require_regular_file(args.vulkan_loader, "Vulkan loader")
        moltenvk = require_regular_file(args.moltenvk, "MoltenVK library")
        mac_binaries = [executable, loader, moltenvk, *runtime]
        if any(item.suffix != ".dylib" for item in mac_binaries[1:]):
            raise PackageError("macOS Vulkan and runtime dependencies must be .dylib files")
        snapshots = validate_macos_package([
            (executable, PurePosixPath("mc_main")),
            (loader, PurePosixPath("lib/libvulkan.1.dylib")),
            (moltenvk, PurePosixPath("lib/libMoltenVK.dylib")),
            *((item, PurePosixPath("lib") / item.name) for item in runtime),
        ])
        # Volk first tries libvulkan.dylib and then this stable loader ABI name.
        # Snapshotting bytes before inspection keeps archive hashes bound to the
        # exact binaries validated by lipo and otool.
        entries.extend(
            ArchiveEntry(location.as_posix(), data, 0o755)
            for _, location, data in snapshots
        )
        entries.append(ArchiveEntry("MinecraftClone.command", macos_launcher(), 0o755))
        entries.append(ArchiveEntry("vulkan/icd.d/MoltenVK_icd.json", sanitized_icd(args.icd_json), 0o644))
        runtime_config = {
            "launcher": "MinecraftClone.command",
            "vulkan_loader": "lib/libvulkan.1.dylib",
            "moltenvk": "lib/libMoltenVK.dylib",
            "icd": "vulkan/icd.d/MoltenVK_icd.json",
        }

    reject_duplicate_names(entries)
    if args.platform == "windows":
        validate_windows_archive_names(entries)
    manifest = {
        "schema": 1,
        "platform": args.platform,
        "version": args.version,
        "source_commit": args.source_commit,
        "toolchain_evidence": evidence,
        "runtime": runtime_config,
        "licenses": sorted(license_names),
        "files": [
            {"path": entry.name, "sha256": sha256_bytes(entry.bytes()), "bytes": len(entry.bytes())}
            for entry in sorted(entries, key=lambda item: item.name)
        ],
    }
    entries.append(ArchiveEntry("manifest.json", canonical_json(manifest), 0o644))
    return entries, manifest


def write_zip(destination: BinaryIO, entries: Sequence[ArchiveEntry]) -> None:
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9, strict_timestamps=True) as archive:
        for entry in sorted(entries, key=lambda item: item.name):
            info = zipfile.ZipInfo(entry.name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (stat.S_IFREG | entry.mode) << 16
            archive.writestr(info, entry.bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)


def write_tar_gz(destination: BinaryIO, entries: Sequence[ArchiveEntry]) -> None:
    with gzip.GzipFile(filename="", mode="wb", fileobj=destination, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as archive:
            for entry in sorted(entries, key=lambda item: item.name):
                data = entry.bytes()
                info = tarfile.TarInfo(entry.name)
                info.size = len(data)
                info.mode = entry.mode
                info.mtime = 0
                info.uid = 0
                info.gid = 0
                info.uname = ""
                info.gname = ""
                archive.addfile(info, fileobj=io.BytesIO(data))


def output_path(value: Path, overwrite: bool) -> Path:
    value = value.absolute()
    current = Path(value.anchor)
    for part in value.parent.parts[1:]:
        current /= part
        if current.is_symlink():
            raise PackageError(f"output parent ancestry must not contain a symlink: {current}")
    if value.is_symlink() or (value.exists() and not overwrite):
        raise PackageError(f"output already exists or is a symlink: {value}; use --overwrite for a regular file")
    if not value.parent.is_dir():
        raise PackageError(f"output parent directory does not exist: {value.parent}")
    return value


def require_output_unchanged(destination: Path, overwrite: bool) -> None:
    output_path(destination, overwrite)
    if destination.is_symlink() or (destination.exists() and not overwrite):
        raise PackageError(
            f"output appeared or became a symlink: {destination}; use --overwrite for a regular file"
        )


def atomic_write_bytes(destination: Path, data: bytes, overwrite: bool) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        suffix=".tmp",
        dir=destination.parent,
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
        require_output_unchanged(destination, overwrite)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def write_desktop(args: argparse.Namespace) -> dict[str, Any]:
    evidence = verify_common(args)
    if (args.platform, args.format) not in {("macos", "tar.gz"), ("windows", "zip")}:
        raise PackageError("macOS requires --format tar.gz and Windows requires --format zip")
    output = output_path(args.output, args.overwrite)
    suffix = ".tar.gz" if args.format == "tar.gz" else ".zip"
    if not str(output).endswith(suffix):
        raise PackageError(f"output for {args.format} must end with {suffix}")
    entries, manifest = desktop_entries(args, evidence)
    sidecar = output_path(output.with_name(output.name + ".manifest.json"), args.overwrite)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.",
        suffix=".tmp",
        dir=output.parent,
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w+b") as stream:
            if args.format == "zip":
                write_zip(stream, entries)
            else:
                write_tar_gz(stream, entries)
            stream.flush()
            stream.seek(0)
            archive_sha256 = hashlib.sha256(stream.read()).hexdigest()
        require_output_unchanged(output, args.overwrite)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    result = {
        "kind": "desktop-package",
        "platform": args.platform,
        "archive": output.name,
        "archive_sha256": archive_sha256,
        "runtime_manifest_sha256": sha256_bytes(canonical_json(manifest)),
    }
    atomic_write_bytes(sidecar, canonical_json(result), args.overwrite)
    return result


def write_android_evidence(args: argparse.Namespace) -> dict[str, Any]:
    evidence = verify_common(args)
    apk, apk_data = snapshot_regular_file(args.apk, "APK")
    if apk.suffix.lower() != ".apk":
        raise PackageError("APK input must have an .apk suffix")
    if args.api_level < 1:
        raise PackageError("--api-level must be positive")
    if not args.abi or any(not re.fullmatch(r"[a-z0-9_-]+", value) for value in args.abi):
        raise PackageError("--abi requires one or more Android ABI names")
    declared_abis = set(args.abi)
    try:
        with zipfile.ZipFile(io.BytesIO(apk_data)) as archive:
            native_libraries: dict[str, set[str]] = {}
            native_paths: set[str] = set()
            for entry in archive.infolist():
                native_path = PurePosixPath(entry.filename)
                parts = native_path.parts
                if entry.is_dir() or not entry.filename.endswith(".so") or not parts or parts[0] != "lib":
                    continue
                if len(parts) != 3 or native_path.as_posix() != entry.filename or entry.filename in native_paths:
                    raise PackageError(f"APK contains an invalid or duplicate native library path: {entry.filename}")
                native_paths.add(entry.filename)
                abi = parts[1]
                if not re.fullmatch(r"[a-z0-9_-]+", abi):
                    raise PackageError(f"APK contains an invalid native ABI path: {entry.filename}")
                with archive.open(entry) as stream:
                    header = stream.read(64)
                actual_abi = android_elf_abi(header, entry.filename)
                if actual_abi != abi:
                    raise PackageError(
                        f"APK native library architecture {actual_abi} does not match directory ABI {abi}: "
                        f"{entry.filename}"
                    )
                native_libraries.setdefault(abi, set()).add(parts[2])
    except (RuntimeError, zipfile.BadZipFile) as error:
        raise PackageError(f"APK input is not a readable package: {error}") from error
    packaged_abis = set(native_libraries)
    if packaged_abis != declared_abis:
        raise PackageError(
            "APK native ABI set does not match --abi: packaged="
            + ",".join(sorted(packaged_abis)) + " declared=" + ",".join(sorted(declared_abis))
        )
    for abi in sorted(declared_abis):
        if ANDROID_NATIVE_LIBRARY not in native_libraries[abi]:
            raise PackageError(f"APK is missing lib/{abi}/{ANDROID_NATIVE_LIBRARY}")
    output = output_path(args.output, args.overwrite)
    result = {
        "schema": 1,
        "kind": "android-apk-evidence",
        "apk": {"name": apk.name, "bytes": len(apk_data), "sha256": sha256_bytes(apk_data)},
        "platform": "android",
        "version": args.version,
        "source_commit": args.source_commit,
        "source_exactness": args.source_exactness,
        "api_level": args.api_level,
        "abis": sorted(declared_abis),
        "signing": {"classification": args.signing, "identity": "not asserted"},
        "toolchain_evidence": evidence,
        "preservation": "APK bytes are not copied, transformed, resigned, or uploaded by this command.",
    }
    serialized = canonical_json(result)
    atomic_write_bytes(output, serialized, args.overwrite)
    result["evidence"] = output.name
    result["evidence_sha256"] = sha256_bytes(serialized)
    return result


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)

    desktop = commands.add_parser("desktop", help="create a deterministic relocatable desktop archive")
    desktop.add_argument("--platform", choices=("macos", "windows"), required=True)
    desktop.add_argument("--format", choices=("tar.gz", "zip"), required=True)
    desktop.add_argument("--executable", type=Path, required=True)
    desktop.add_argument("--shaders", type=Path, required=True)
    desktop.add_argument("--runtime", type=Path, action="append", default=[])
    desktop.add_argument("--license", type=Path, action="append", default=[])
    desktop.add_argument("--vulkan-loader", type=Path)
    desktop.add_argument("--moltenvk", type=Path)
    desktop.add_argument("--icd-json", type=Path)

    android = commands.add_parser("android", help="write evidence for an existing Android APK without modifying it")
    android.add_argument("--apk", type=Path, required=True)
    android.add_argument("--api-level", type=int, required=True)
    android.add_argument("--abi", action="append", default=[])
    android.add_argument("--signing", choices=("development", "unknown"), required=True)
    android.add_argument("--source-exactness", choices=("exact", "unknown"), required=True)

    for command in (desktop, android):
        command.add_argument("--version", required=True)
        command.add_argument("--source-commit", required=True)
        command.add_argument("--toolchain-evidence", type=Path, required=True)
        command.add_argument("--output", type=Path, required=True)
        command.add_argument("--overwrite", action="store_true")
    return root


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        result = write_desktop(args) if args.command == "desktop" else write_android_evidence(args)
    except (OSError, PackageError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
