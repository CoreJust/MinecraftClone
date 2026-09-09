#!/usr/bin/env python3
"""Read-only environment doctor and safe local AI hook installer."""

from __future__ import annotations

import argparse
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path
from typing import Sequence


def git(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", *args], cwd=root, text=True, capture_output=True, check=False)


def active_legacy_hooks(root: Path) -> list[Path]:
    resolved = git(root, "rev-parse", "--git-path", "hooks")
    if resolved.returncode:
        return []
    hooks_dir = Path(resolved.stdout.strip())
    if not hooks_dir.is_absolute():
        hooks_dir = root / hooks_dir
    if not hooks_dir.is_dir():
        return []
    return sorted(path for path in hooks_dir.iterdir() if path.is_file() and not path.name.endswith(".sample"))


def install_hooks(root: Path) -> int:
    configured = git(root, "config", "--get", "--includes", "core.hooksPath")
    current = configured.stdout.strip() if configured.returncode == 0 else ""
    if current and current != ".githooks":
        print(f"Refusing to replace existing core.hooksPath: {current}", file=sys.stderr)
        return 1
    if not current:
        legacy = active_legacy_hooks(root)
        if legacy:
            print("Refusing to hide existing hooks: " + ", ".join(path.name for path in legacy), file=sys.stderr)
            return 1
    hook_dir = root / ".githooks"
    hooks = [hook_dir / name for name in ("pre-commit", "pre-push", "commit-msg", "post-commit", "pre-merge-commit")]
    if any(not hook.is_file() or not hook.stat().st_mode & stat.S_IXUSR for hook in hooks):
        print(
            "Executable .githooks/pre-commit, .githooks/pre-push, .githooks/commit-msg, .githooks/post-commit, and .githooks/pre-merge-commit are required",
            file=sys.stderr,
        )
        return 1
    if current == ".githooks":
        print("Hooks are already installed.")
        return 0
    configured = git(root, "config", "--local", "core.hooksPath", ".githooks")
    if configured.returncode:
        print(configured.stderr.strip() or "Unable to set core.hooksPath", file=sys.stderr)
        return 1
    print("Installed local hooks through core.hooksPath=.githooks")
    return 0


def doctor(root: Path) -> int:
    branch = git(root, "branch", "--show-current")
    print(f"Python: {sys.version.split()[0]} ({sys.executable})")
    print(f"CMake: {shutil.which('cmake') or 'missing'}")
    print(f"Ninja: {shutil.which('ninja') or 'missing'}")
    print(f"VCPKG_ROOT: {'set' if os.environ.get('VCPKG_ROOT') else 'missing'}")
    print(f"Branch: {branch.stdout.strip() if branch.returncode == 0 else 'unavailable'}")
    return int(sys.version_info < (3, 12) or not shutil.which("cmake") or not shutil.which("ninja"))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-hooks", action="store_true", help="set the local hooks path when safe")
    parser.add_argument("--root", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    root = (args.root or Path(__file__).resolve().parents[1]).resolve()
    return install_hooks(root) if args.install_hooks else doctor(root)


if __name__ == "__main__":
    raise SystemExit(main())
