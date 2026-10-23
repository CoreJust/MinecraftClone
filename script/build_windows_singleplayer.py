#!/usr/bin/env python3
"""Build one offline Windows GUI executable from a reviewed game runtime closure."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

import package_snapshot as packages

MAGIC = b"MCSINGLE"
SCHEMA = 1
ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / "script/windows-singleplayer-launcher"
REQUIRED_SHADER_PATHS = frozenset(f"shaders/{name}.spv" for name in (
    "grid.mesh", "player.mesh", "grid.vert", "player.vert", "player.frag", "stone.vert",
    "stone_indexed_block.vert", "stone.frag", "stone_solid.frag", "sky.vert", "sky.frag",
    "trivial.frag", "text.vert", "text.frag", "map.vert", "map.frag",
))


def collect_entries(runtime: Path, assets: Path, scenarios: Path) -> list[packages.ArchiveEntry]:
    runtime = packages.require_directory(runtime, "reviewed runtime directory")
    entries = []
    packages.add_file(entries, runtime / "mc_main.exe", "mc_main.exe", "game executable")
    dlls = sorted(item for item in runtime.iterdir() if item.suffix.lower() == ".dll")
    if not dlls:
        raise packages.PackageError("reviewed runtime directory contains no runtime DLLs")
    for dll in dlls:
        packages.add_file(entries, dll, dll.name, "runtime DLL")
    shaders = [entry for entry in packages.files_in_tree(runtime / "shaders", "shaders", "shaders")
               if entry.name.endswith(".spv")]
    shader_paths = {entry.name for entry in shaders}
    if shader_paths != REQUIRED_SHADER_PATHS:
        missing = sorted(REQUIRED_SHADER_PATHS - shader_paths)
        unexpected = sorted(shader_paths - REQUIRED_SHADER_PATHS)
        raise packages.PackageError(
            f"reviewed runtime shader paths differ: missing={missing}, unexpected={unexpected}"
        )
    if any(entry.data[:4] != b"\x03\x02\x23\x07" for entry in shaders):
        raise packages.PackageError("reviewed runtime contains an invalid SPIR-V shader")
    entries.extend(shaders)
    entries.extend(packages.files_in_tree(assets, "assets", "assets"))
    entries.extend(packages.files_in_tree(scenarios, "scenarios", "scenarios"))
    packages.add_file(entries, ROOT / "LICENSE", "licenses/MinecraftClone-LICENSE", "game license")
    for name in ("DEBUG_HUD_ATTRIBUTION.md", "TAMSYN_LICENSE.txt"):
        packages.add_file(entries, ROOT / "src/client/render/shaders" / name, f"licenses/hud/{name}", "HUD license")
    if (runtime / "licenses").exists():
        entries.extend(packages.files_in_tree(runtime / "licenses", "licenses/runtime", "runtime licenses"))
    packages.validate_windows_archive_names(entries)
    return entries


def write_bundle(entries: list[packages.ArchiveEntry], output: Path) -> dict:
    if not entries or len(entries) > 4096:
        raise packages.PackageError("bundle must contain between 1 and4096 files")
    for entry in entries:
        if len(entry.name.encode("utf-8")) > 1024:
            raise packages.PackageError("bundle path exceeds1024 encoded bytes")
        if packages.archive_name(entry.name) != entry.name:
            raise packages.PackageError(f"noncanonical bundle path: {entry.name}")
    packages.validate_windows_archive_names(entries)
    output = packages.output_path(output, False)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
    temporary = Path(temporary_name)
    manifest = []
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(MAGIC + struct.pack("<II", SCHEMA, len(entries)))
            for entry in sorted(entries, key=lambda item: item.name):
                name = entry.name.encode("utf-8")
                digest = hashlib.sha256(entry.data).digest()
                stream.write(struct.pack("<IQ", len(name), len(entry.data)) + digest + name + entry.data)
                manifest.append({"path": entry.name, "bytes": len(entry.data), "sha256": digest.hex()})
        with temporary.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        bundle_bytes = temporary.stat().st_size
        packages.require_output_unchanged(output, False)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    return {"schema": "mc.singleplayer-bundle.v1", "bundle_sha256": digest,
            "bundle_bytes": bundle_bytes, "files": manifest,
            "maximum_build_extra_bytes": bundle_bytes * 3 + 4_194_304}


def preflight_outputs(output: Path) -> tuple[Path, Path]:
    output = output.absolute()
    if output.suffix.lower() != ".exe":
        raise packages.PackageError("output must be a new .exe path; preserve existing deliveries")
    sidecar = output.with_suffix(".manifest.json")
    for destination in (output, sidecar):
        if destination.is_symlink() or destination.exists():
            raise packages.PackageError(f"output already exists or is a symlink: {destination}")
    for parent in output.parents:
        if parent.is_symlink():
            raise packages.PackageError(f"output parent ancestry must not contain a symlink: {parent}")
    return output, sidecar


def verify_gui_executable(executable: Path) -> None:
    with executable.open("rb") as stream:
        dos = stream.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise packages.PackageError("launcher has no Windows executable header")
        stream.seek(struct.unpack_from("<I", dos, 60)[0])
        pe = stream.read(96)
    if (len(pe) != 96 or pe[:4] != b"PE\0\0" or struct.unpack_from("<H", pe, 4)[0] != 0x8664
            or struct.unpack_from("<H", pe, 24)[0] != 0x20b
            or struct.unpack_from("<H", pe, 92)[0] != 2):
        raise packages.PackageError("launcher must be an x64 Windows GUI executable without a console subsystem")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--assets", type=Path, default=ROOT / "assets")
    parser.add_argument("--scenarios", type=Path, default=ROOT / "scenarios")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-extra-bytes", type=int, required=True)
    parser.add_argument("--expected-game-sha256", required=True)
    parser.add_argument("--source-identity", required=True)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    try:
        output, sidecar = preflight_outputs(args.output)
        entries = collect_entries(args.runtime, args.assets, args.scenarios)
        game = next(entry for entry in entries if entry.name == "mc_main.exe")
        if hashlib.sha256(game.data).hexdigest() != args.expected_game_sha256:
            raise packages.PackageError("game executable does not match its reviewed SHA256")
        estimated_bytes = 16 + sum(44 + len(item.name.encode("utf-8")) + len(item.data) for item in entries)
        if estimated_bytes >= 2**32:
            raise packages.PackageError("bundle exceeds the Windows resource size limit")
        if estimated_bytes * 3 + 4_194_304 > args.max_extra_bytes:
            raise packages.PackageError("bundle build would exceed the supplied extra-disk budget")
        output.parent.mkdir(parents=True, exist_ok=True)
        packages.output_path(output, False)
        packages.output_path(sidecar, False)
        with tempfile.TemporaryDirectory(prefix="singleplayer-build-", dir=output.parent) as temporary:
            work = Path(temporary)
            bundle = work / "payload.bin"
            manifest = write_bundle(entries, bundle)
            manifest.update(source_identity=args.source_identity,
                            game_sha256=args.expected_game_sha256,
                            cache_namespace=r"%LOCALAPPDATA%\MinecraftClone\SinglePlayer",
                            child_arguments=[], child_console=False)
            if args.prepare_only:
                manifest["prepared_only"] = True
            else:
                if sys.platform != "win32" or not shutil.which("cl") or not shutil.which("rc"):
                    raise packages.PackageError("build requires the existing native Windows MSVC/SDK shell")
                resource = work / "payload.rc"
                resource.write_text(
                    f'101 RCDATA "{bundle.as_posix()}"\n'
                    f'1 24 "{(LAUNCHER / "launcher.manifest").as_posix()}"\n', encoding="utf-8")
                subprocess.run(["rc", "/nologo", "/fo", str(work / "payload.res"), str(resource)], check=True)
                compiled = work / "MinecraftClone.exe"
                subprocess.run(["cl", "/nologo", "/std:c++20", "/O2", "/MT", "/EHsc", "/W4", "/WX",
                                "/DUNICODE", "/D_UNICODE", "/DNOMINMAX", "/D_WIN32_WINNT=0x0A00", str(LAUNCHER / "launcher.cpp"),
                                str(work / "payload.res"), f"/Fe:{compiled}",
                                "/link", "/SUBSYSTEM:WINDOWS", "/INCREMENTAL:NO", "bcrypt.lib", "shell32.lib", "ole32.lib", "user32.lib"],
                               cwd=work, check=True)
                verify_gui_executable(compiled)
                with compiled.open("rb") as source:
                    manifest["launcher_sha256"] = hashlib.file_digest(source, "sha256").hexdigest()
                manifest["launcher_bytes"] = compiled.stat().st_size
                manifest["retained_cache_bytes"] = sum(entry["bytes"] for entry in manifest["files"])
            packages.require_output_unchanged(output, False)
            packages.require_output_unchanged(sidecar, False)
            if not args.prepare_only:
                os.rename(compiled, output)
            packages.atomic_write_bytes(
                sidecar, (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"), False
            )
            print(json.dumps(manifest, indent=2, sort_keys=True), flush=True)
        return 0
    except (OSError, packages.PackageError, subprocess.CalledProcessError) as error:
        print(f"Single-player package failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
