import os
import subprocess
import glob
import re
import copy
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Tuple, Callable, Dict, Any

from script.colored_print import *
from script.check_options import CODE_EXTS, SHADER_EXTS

CheckFunc = Callable[[Dict[str, Any]], Tuple[bool, str]]
FileCheckFunc = Callable[[Dict[str, Any], str], Tuple[bool, str]]
checks: List[Tuple[str, CheckFunc]] = []

def register_check(description: str):
    def decorator(func: CheckFunc):
        checks.append((description, func))
        return func
    return decorator

FileFinder = Callable[[Dict[str, Any]], [str, str]]

def read_file(path: str) -> str:
    with open(path, 'r', encoding='utf-8') as f:
        return f.read()

class FILES:
    def PROJECT_INFO(ctx):
        base = 'src/shared/include/shared/ProjectInfo'
        candidates = glob.glob(base + '.*')
        if not candidates:
            return None, 'ProjectInfo'
        for c in candidates:
            if c.endswith('.hpp'):
                return c, 'ProjectInfo'
        return candidates[0], 'ProjectInfo'

    def HISTORY(ctx):
        return ctx.get('history_file'), 'History'

    def README(ctx):
        return 'README.md', 'README.md'

    def ROOT_CMAKELISTS(ctx):
        return 'CMakeLists.txt', 'CMakeLists.txt'

    def VCPKG(ctx):
        return 'vcpkg.json', 'vcpkg.json'

def register_file_check(finder: FileFinder, description: str):
    def decorator(func: FileCheckFunc):
        def runner(ctx):
            path, file_name = finder(ctx)
            if not path:
                return False, file_name + " file not found"
            content = read_file(path)
            return func(ctx, content)
        checks.append((description, runner))
        return runner
    return decorator

def run_checks(ctx: Dict[str, Any]) -> bool:
    all_passed = True
    for idx, (desc, func) in enumerate(checks, 1):
        print(f"[{idx}/{len(checks)}] {desc} ... ", end="", flush=True)
        try:
            passed, msg = func(ctx)
        except Exception as e:
            passed, msg = False, f"Exception: {e}"
        if passed:
            print_pass()
        else:
            print_fail(msg)
            all_passed = False
    return all_passed

def glob_recursive(root: str, pattern: str) -> List[str]:
    return [str(p) for p in Path(root).rglob(pattern)]

def git_output(*args) -> str:
    result = subprocess.run(['git'] + list(args), capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Git command failed: {' '.join(args)}\n{result.stderr}")
    return result.stdout.strip()

@dataclass
class SourceSpec:
    extensions: Set[str]
    directories: List[str] = field(default_factory=lambda: ['tests', 'src'])
    recursive: bool = True
    read_mode: str = 'r'

    def binary(self):
        result = copy.deepcopy(self)
        result.read_mode = 'rb'
        return result

HEADER_FILES = SourceSpec({'.hpp'})
CODE_FILES = SourceSpec(CODE_EXTS)
SHADER_FILES = SourceSpec(SHADER_EXTS)

def collect_files(spec: SourceSpec) -> List[str]:
    all_files = []
    for d in spec.directories:
        if os.path.isdir(d):
            for ext in spec.extensions:
                pattern = f'**/*{ext}' if spec.recursive else f'*{ext}'
                all_files.extend(glob_recursive(d, pattern))
    return sorted(all_files)

def register_source_check(spec: SourceSpec, description: str):
    def decorator(func: Callable):
        def runner(ctx):
            files = collect_files(spec)
            all_errors = []
            encoding = None if 'b' in spec.read_mode else 'utf-8'
            for fpath in files:
                try:
                    with open(fpath, spec.read_mode, encoding=encoding) as f:
                        content = f.read()
                    errs = func(ctx, fpath, content)
                    for e in errs:
                        all_errors.append(f"{fpath}: {e}")
                except Exception as ex:
                    all_errors.append(f"{fpath}: Exception: {ex}")
            if all_errors:
                return False, "\n  ".join(all_errors)
            return True, ""
        checks.append((description, runner))
        return func
    return decorator

def strip_cpp_comments(text: str) -> str:
    text = re.sub(r'//.*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    return text

def line_check(lines: List[str], predicate, fmt) -> List[str]:
    errors = []
    for i, line in enumerate(lines, 1):
        if predicate(line):
            errors.append(f"line {i}: {fmt(line)}")
    return errors

def content_find(content: str, pattern: str, error_msg: str, flags=0) -> List[str]:
    if re.search(pattern, content, flags):
        return [error_msg]
    return []
