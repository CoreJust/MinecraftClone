#!/usr/bin/env python3
"""Analyze an owned translation unit before invoking its actual compiler."""
from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
PLATFORM_INCLUDE_ROOTS = []
STANDARD_HEADERS = set("algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes climits clocale cmath compare complex concepts condition_variable coroutine csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype deque exception execution expected filesystem format forward_list functional future initializer_list iomanip ios iosfwd iostream istream iterator latch limits list map memory memory_resource mutex new numbers numeric optional ostream print queue random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location span sstream stack stdexcept stop_token streambuf string string_view syncstream system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector version assert.h ctype.h errno.h fenv.h float.h inttypes.h limits.h locale.h math.h setjmp.h signal.h stdarg.h stdbool.h stddef.h stdint.h stdio.h stdlib.h string.h time.h uchar.h wchar.h wctype.h".split())
STANDARD_HEADERS.update({"locale", "codecvt", "fstream", "strstream", "stacktrace", "stdfloat", "mdspan"})
sys.path.insert(0, str(ROOT / "script"))
from ai_analysis_matrix import project_analysis_input_sha256


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parse_xml(data, cwd, root):
    document = ET.fromstring(data)
    if document.tag != "results" or document.find("errors") is None:
        raise ValueError("missing cppcheck results/errors XML")
    diagnostics = []
    for error in document.find("errors"):
        severity = error.get("severity")
        identifier = error.get("id")
        if error.tag != "error" or not identifier or severity not in {"error", "warning", "performance", "portability", "style", "information", "debug"}:
            raise ValueError("malformed or unknown cppcheck diagnostic")
        location = error.find("location")
        filename = location.get("file", "") if location is not None else ""
        resolved = (cwd / filename).resolve() if filename else None
        primary = bool(resolved and any(resolved.is_relative_to(root / directory) for directory in ("src", "tests")))
        message = error.get("verbose", error.get("msg", ""))
        missing = re.search(r'Include file: [<"]([^>"\n]+)[>"] not found', message)
        header = missing.group(1) if missing else ""
        platform_header = False
        for include_root in PLATFORM_INCLUDE_ROOTS:
            if (include_root / header).is_file():
                platform_header = True
            if "/" in header:
                framework, name = header.split("/", 1)
                sdk = next((parent for parent in include_root.parents if parent.name.endswith(".sdk")), None)
                if sdk and (sdk / "System/Library/Frameworks" / (framework + ".framework") / "Headers" / name).is_file():
                    platform_header = True
        modeled_header = platform_header or (identifier == "missingIncludeSystem" and header in STANDARD_HEADERS)
        configuration = identifier in {"syntaxError", "internalError", "preprocessorErrorDirective", "unknownMacro", "cppcheckError", "configurationNotChecked"} or (identifier in {"missingInclude", "missingIncludeSystem"} and not modeled_header) or severity == "error" and not filename
        blocking = configuration or (primary and severity in {"error", "warning", "performance", "portability"} and identifier not in {"uninitMemberVarNoCtor", "passedByValue"})
        diagnostics.append({"id": identifier, "severity": severity, "file": str(resolved) if resolved else "", "line": int(location.get("line", "0")) if location is not None else 0, "message": message, "primary": primary, "blocking": blocking})
    return diagnostics


def run(argv):
    start = time.monotonic()
    result = subprocess.run(argv, capture_output=True)
    return result, round((time.monotonic() - start) * 1000)


def analyzer_arguments(options, compiler, source, cwd):
    args = list(options)
    if source.is_relative_to(ROOT / "tests"):
        args.append("--library=googletest")
    index = 1
    while index < len(compiler):
        argument = compiler[index]
        if argument.startswith("-std="):
            args.append("--std=" + argument.split("=", 1)[1])
        elif argument in {"-I", "-isystem", "-D", "-U"}:
            index += 1
            value = compiler[index]
            args.append(("-I" if argument == "-isystem" else argument) + value)
        elif argument.startswith(("-I", "-D", "-U")):
            args.append(argument)
        index += 1
    # Query the compiler's actual target; never infer the OS from the host.
    macro_args = [compiler[0]]
    index = 1
    while index < len(compiler):
        argument = compiler[index]
        if argument in {
            "-target", "--target", "-isysroot", "--sysroot", "-arch", "-D", "-U",
            "-I", "-isystem", "-iquote", "-idirafter", "-F", "-iframework", "-resource-dir",
        }:
            macro_args.extend(compiler[index:index + 2]); index += 2; continue
        if argument in {"-nostdinc", "-nostdinc++"} or argument.startswith(("--target=", "--sysroot=", "-std=", "-stdlib=", "-D", "-U", "-I", "-F")):
            macro_args.append(argument)
        index += 1
    result = subprocess.run(macro_args + ["-dM", "-E", "-v", "-x", "c++", "-"], input=b"", capture_output=True)
    if result.returncode:
        raise ValueError("compiler target macro query failed")
    macros = result.stdout.decode().splitlines()
    PLATFORM_INCLUDE_ROOTS.clear()
    compiler_search_paths = []
    in_search_paths = False
    for line in result.stderr.decode().splitlines():
        if line.strip() == "#include <...> search starts here:":
            in_search_paths = True
            continue
        if in_search_paths and line.strip() == "End of search list.":
            break
        if not in_search_paths:
            continue
        candidate = Path(line.strip().removesuffix(" (framework directory)"))
        if not candidate.is_dir():
            continue
        candidate = candidate.resolve()
        compiler_search_paths.append(candidate)
        if ".sdk/" in str(candidate) or "/lib/clang/" in str(candidate):
            PLATFORM_INCLUDE_ROOTS.append(candidate)
    # Cppcheck's built-in model is preferred to analyzing libc++'s implementation
    # headers. Remove only roots confirmed in Clang's search order by libc++'s
    # version marker; leave project, dependency, and compiler arguments intact.
    libcxx_roots = set()
    for include_root in compiler_search_paths:
        config = include_root / "__config"
        try:
            content = config.read_bytes()
        except OSError:
            continue
        if re.search(rb"^\s*#\s*define\s+_LIBCPP_VERSION\b", content, re.MULTILINE):
            libcxx_roots.add(include_root)
    if libcxx_roots:
        filtered = []
        index = 0
        while index < len(args):
            argument = args[index]
            if argument == "-I" and index + 1 < len(args):
                include_root = Path(args[index + 1])
                if not include_root.is_absolute():
                    include_root = cwd / include_root
                if include_root.resolve() in libcxx_roots:
                    index += 2
                    continue
            elif argument.startswith("-I") and argument != "-I":
                include_root = Path(argument[2:])
                if not include_root.is_absolute():
                    include_root = cwd / include_root
                if include_root.resolve() in libcxx_roots:
                    index += 1
                    continue
            filtered.append(argument)
            index += 1
        args = filtered
    if any(line.startswith("#define __APPLE__ ") for line in macros):
        result = subprocess.run(macro_args + ["-dM", "-E", "-x", "c++", "-include", "TargetConditionals.h", "-"], input=b"", capture_output=True)
        if result.returncode:
            raise ValueError("compiler Apple target macro query failed")
        macros = result.stdout.decode().splitlines()
    for line in macros:
        parts = line.split(maxsplit=2)
        if len(parts) >= 2 and "(" not in parts[1]:
            args.append("-D" + parts[1] + "=" + (parts[2] if len(parts) == 3 else "1"))
    # Clang builtins are not emitted by -dM. Query each feature actually named
    # by project/dependency headers and model the same lookup with token pasting.
    builtin_names = {name: set() for name in ("__has_feature", "__has_extension", "__has_attribute", "__has_cpp_attribute", "__has_builtin")}
    include_dirs = []
    for argument in args:
        if argument.startswith("-I"):
            include_dirs.append(Path(argument[2:]))
    for directory in include_dirs:
        directory = directory if directory.is_absolute() else cwd / directory
        if directory.is_dir():
            for header_path in directory.rglob("*"):
                if header_path.suffix in {".h", ".hpp", ".inl"} and header_path.is_file():
                    text = header_path.read_text(errors="replace")
                    for builtin, names in builtin_names.items():
                        names.update(re.findall(re.escape(builtin) + r"\s*\(\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)", text))
    if any(builtin_names.values()):
        query = "\n".join(f"MC_CPPCHECK_QUERY KEY{builtin} {name} {builtin}({name})" for builtin, names in sorted(builtin_names.items()) for name in sorted(names))
        result = subprocess.run(macro_args + ["-E", "-P", "-x", "c++", "-"], input=query.encode(), capture_output=True)
        if result.returncode:
            raise ValueError("compiler builtin feature query failed")
        values = {}
        for line in result.stdout.decode().splitlines():
            if line.startswith("MC_CPPCHECK_QUERY "):
                _, builtin, name, value = line.split(maxsplit=3)
                builtin = builtin.removeprefix("KEY")
                if not re.fullmatch(r"[0-9]+[uUlL]*", value) or name not in builtin_names.get(builtin, set()):
                    raise ValueError("unrecognized compiler builtin query result")
                values[(builtin, name)] = value
        expected = {(builtin, name) for builtin, names in builtin_names.items() for name in names}
        if set(values) != expected:
            raise ValueError("incomplete compiler builtin query results")
        for builtin, names in sorted(builtin_names.items()):
            if names:
                prefix = "MC_CPPCHECK_" + builtin.strip("_").upper() + "_"
                args.append(f"-D{builtin}(x)={prefix}##x")
                args.extend(f"-D{prefix}{name}={values[(builtin, name)]}" for name in sorted(names))
    args.append(str(source))
    return args


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--":
        argv.pop(0)
    split = argv.index("--")
    options, compiler = argv[:split], argv[split + 1:]
    cwd = Path.cwd().resolve()
    source = Path(compiler[compiler.index("-c") + 1])
    source = (cwd / source).resolve()
    relative = source.relative_to(ROOT).as_posix()
    if relative.startswith("build/") and any(part in {"CMakeScratch", "CMakeTmp"} for part in source.parts):
        return subprocess.call(compiler)
    if relative.split("/", 1)[0] not in {"src", "tests"}:
        raise ValueError("unexpected nonowned compiler source")
    receipt = {"schema": 1, "gate": "cppcheck-per-tu", "status": "fail", "failure_stage": "configuration", "source": {"absolute": str(source), "relative": relative, "sha256": digest(source.read_bytes())}, "cwd": str(cwd), "project_input_sha256": project_analysis_input_sha256(ROOT), "cppcheck": {"argv": [], "version": "", "returncode": None, "elapsed_ms": 0}, "compiler": {"argv": compiler, "returncode": None, "elapsed_ms": 0}, "diagnostics": []}
    print("CPPCHECK_GATE_PROGRESS " + json.dumps({"source": relative, "phase": "analyze"}), flush=True)
    try:
        checker = analyzer_arguments(options, compiler, source, cwd)
        receipt["cppcheck"]["argv"] = checker
        version, _ = run([options[0], "--version"])
        if version.returncode:
            raise ValueError("cppcheck version query failed")
        receipt["cppcheck"]["version"] = version.stdout.decode().strip()
        result, elapsed = run(checker)
        xml = result.stderr
        receipt["cppcheck"].update(returncode=result.returncode, elapsed_ms=elapsed)
        receipt["diagnostics"] = parse_xml(xml, cwd, ROOT)
        receipt["failure_stage"] = "analyzer"
        if result.returncode == 0 and not any(item["blocking"] for item in receipt["diagnostics"]):
            print("CPPCHECK_GATE_PROGRESS " + json.dumps({"source": relative, "phase": "compile"}), flush=True)
            compiled, elapsed = run(compiler)
            sys.stdout.buffer.write(compiled.stdout); sys.stderr.buffer.write(compiled.stderr)
            receipt["compiler"].update(returncode=compiled.returncode, elapsed_ms=elapsed)
            receipt["failure_stage"] = "compiler" if compiled.returncode else None
            receipt["status"] = "fail" if compiled.returncode else "pass"
    except (OSError, ValueError, ET.ParseError) as error:
        xml = locals().get("xml", b"")
        receipt["error"] = str(error)
    identity = {"source_absolute": str(source), "source_sha256": receipt["source"]["sha256"], "cppcheck_argv": receipt["cppcheck"]["argv"], "compiler_argv": compiler, "cwd": str(cwd), "project_input_sha256": receipt["project_input_sha256"]}
    receipt["identity_sha256"] = digest(json.dumps(identity, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode())
    output = ROOT / "build/analysis-cppcheck/gate"
    output.mkdir(parents=True, exist_ok=True)
    xml_path = output / (receipt["identity_sha256"] + ".xml")
    receipt_path = xml_path.with_suffix(".json")
    receipt["raw_report_sha256"] = digest(xml)
    diagnostics = receipt["diagnostics"]
    receipt["summary"] = {"blocking": sum(item["blocking"] for item in diagnostics), "advisory": sum(not item["blocking"] for item in diagnostics), "by_severity": {severity: sum(item["severity"] == severity for item in diagnostics) for severity in sorted({item["severity"] for item in diagnostics})}}
    receipt["artifacts"] = {"root": str(output), "xml": str(xml_path), "receipt": str(receipt_path)}
    for destination, data in ((xml_path, xml), (receipt_path, json.dumps(receipt, sort_keys=True, indent=2).encode())):
        temporary = destination.with_suffix(destination.suffix + f".{os.getpid()}.tmp")
        temporary.write_bytes(data); temporary.replace(destination)
    print("CPPCHECK_GATE_RESULT " + json.dumps(receipt), flush=True)
    return 0 if receipt["status"] == "pass" else (receipt["compiler"]["returncode"] or 2)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, IndexError) as error:
        print("CPPCHECK_GATE_RESULT " + json.dumps({"status": "fail", "failure_stage": "configuration", "error": str(error)}))
        sys.exit(2)
