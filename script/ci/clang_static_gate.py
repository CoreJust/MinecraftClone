#!/usr/bin/env python3
"""Launch one scan-build compilation and fail closed on analyzer failures."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


class GateError(ValueError):
    """The compiler-launcher invocation cannot establish analysis evidence."""


def _executable(value: str) -> str:
    resolved = shutil.which(value)
    if resolved is None:
        raise GateError(f"executable not found: {value}")
    return str(Path(resolved).absolute())


def _argument(arguments: list[str], flag: str) -> Path:
    indices = [index for index, value in enumerate(arguments) if value == flag]
    if len(indices) != 1 or indices[0] + 1 >= len(arguments):
        raise GateError(f"expected exactly one {flag} <file> argument")
    return Path(arguments[indices[0] + 1]).resolve()


def _launcher(target: Path, compiler: str, preserve_nostdinc: bool) -> None:
    # Embed only fixed program inputs, never a shell command or environment dump.
    target.write_text(
        f"#!{sys.executable}\n"
        "import json, os, subprocess, sys\n"
        f"compiler = {compiler!r}\n"
        f"audit = {str(target.parent / 'analysis.jsonl')!r}\n"
        "arguments = sys.argv[1:]\n"
        f"if {preserve_nostdinc!r} and arguments[:1] != ['-cc1']:\n"
        "    arguments = ['-nostdinc++', *arguments]\n"
        "if arguments[:1] == ['-cc1'] and '-analyze' in arguments:\n"
        "    result = subprocess.run([compiler, *arguments], check=False)\n"
        "    with open(audit, 'a', encoding='utf-8') as stream:\n"
        "        stream.write(json.dumps({'argv': [compiler, *arguments], 'returncode': result.returncode}) + '\\n')\n"
        "    sys.exit(result.returncode)\n"
        "os.execv(compiler, [compiler, *arguments])\n",
        encoding="utf-8",
    )
    target.chmod(0o700)


def main(arguments: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if arguments is None else arguments)
    try:
        if not arguments or arguments.pop(0) != "--" or "--" not in arguments:
            raise GateError("expected -- scan-build --status-bugs -v -- compiler <arguments>")
        separator = arguments.index("--")
        tool, compiler_argv = arguments[:separator], arguments[separator + 1:]
        if len(tool) != 3 or tool[1:] != ["--status-bugs", "-v"] or not compiler_argv:
            raise GateError("expected scan-build --status-bugs -v and a compiler command")
        scan_build = _executable(tool[0])
        compiler = _executable(compiler_argv[0])
        source = _argument(compiler_argv, "-c")
        output = _argument(compiler_argv, "-o")
        if not source.is_file() or source == output or output.parent == source.parent:
            raise GateError("expected an existing source and a separate build object directory")
        output.parent.mkdir(parents=True, exist_ok=True)
        report_root = Path(tempfile.mkdtemp(prefix="clang-static-", dir=output.parent))
        launcher = report_root / "clang++"
        _launcher(launcher, compiler, "-nostdinc++" in compiler_argv)
        command = [scan_build, "--status-bugs", "-v", "--keep-empty", "-o", str(report_root / "reports"),
                   f"--use-analyzer={launcher}", f"--use-cc={compiler}", f"--use-c++={compiler}",
                   compiler, *compiler_argv[1:]]
        print("CLANG_STATIC_GATE_COMMAND " + json.dumps(command), flush=True)
        log = report_root / "scan-build.log"
        with log.open("wb") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=False)
        audit = report_root / "analysis.jsonl"
        analyses = [json.loads(line) for line in audit.read_text().splitlines()] if audit.is_file() else []
        artifacts = [
            {"relative": target.relative_to(report_root).as_posix(), "size": target.stat().st_size,
             "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}
            for target in sorted(report_root.rglob("*")) if target.is_file()
        ]
        failures = [item["relative"] for item in artifacts
                    if "/failures/" in "/" + item["relative"] and item["relative"].endswith(".info.txt")]
        report_dirs = [target for target in (report_root / "reports").glob("*") if target.is_dir()]
        passed = (result.returncode == 0 and not failures and bool(report_dirs)
                  and bool(analyses) and all(item.get("returncode") == 0 for item in analyses))
        record = {"schema": 1, "source": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                  "compiler": compiler_argv, "command": command, "returncode": result.returncode,
                  "status": "passed" if passed else "failed", "internal_failures": failures,
                  "analyses": analyses, "artifacts": artifacts}
        receipt = report_root / "receipt.json"
        receipt.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(log.read_text(encoding="utf-8", errors="replace"), end="", flush=True)
        print("CLANG_STATIC_GATE_RESULT " + json.dumps({"status": record["status"], "receipt": str(receipt),
                                                       "internal_failures": failures}), flush=True)
        return result.returncode if result.returncode > 0 else (0 if passed else 1)
    except (GateError, OSError, json.JSONDecodeError) as error:
        print(f"FAIL clang-static-gate: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
