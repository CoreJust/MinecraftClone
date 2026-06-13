import os, re
from typing import List, Optional, Tuple
from script.checks_common import *
from script.check_options import *

def _is_self_include(filepath: str, path: str) -> bool:
    if not filepath.endswith('.cpp'):
        return False
    expected = os.path.splitext(os.path.basename(filepath))[0] + '.hpp'
    return os.path.basename(path) == expected

def _parse_include(line: str) -> Optional[Tuple[str, str]]:
    m = re.match(r'\s*#\s*include\s*([<"])([^>"]+)[>"]', line)
    return (m.group(2), m.group(1)) if m else None

def _classify(path: str, quote_char: str, filepath: str) -> str:
    if _is_self_include(filepath, path):
        return 'self'
    if quote_char == '"':
        return 'relative'
        
    if path.startswith(SRC_SUBFOLDERS):
        return 'project'
    lib = path.split('/')[0].split('.')[0]
    return 'std' if lib in STD_HEADERS else 'thirdparty'

def _check_include_order(lines: List[str], filepath: str) -> List[str]:
    errors = []
    entries = []
    for i, raw_line in enumerate(lines):
        line = re.sub(r'//.*', '', raw_line).strip()
        parsed = _parse_include(line)
        if not parsed:
            continue
        path, quote = parsed
        dont = False
        for j in range(i - 1, -1, -1):
            prev = lines[j].strip()
            if prev == '':
                continue
            if f'// DONT_CHECK {DONT_CHECKS["include_order"]}' in prev:
                dont = True
            break
        cat = _classify(path, quote, filepath)
        entries.append((i, path, cat, dont))

    ordered = [(i, p, c) for i, p, c, d in entries if not d]

    # 1. self‑include must be first
    self_entries = [e for e in ordered if e[2] == 'self']
    if self_entries:
        if ordered[0][2] != 'self':
            errors.append(f"line {self_entries[0][0]+1}: corresponding .hpp must be included first")

    # 2. category order & empty lines between categories
    prev_cat, prev_idx = None, None
    for idx, p, cat in ordered:
        if cat != prev_cat:
            if prev_cat is not None:
                # empty line required between categories
                if not any(line.strip() == '' for line in lines[prev_idx+1:idx]):
                    errors.append(f"line {idx+1}: missing empty line between {prev_cat} and {cat}")
                if INCLUDES_ORDER.index(cat) < INCLUDES_ORDER.index(prev_cat):
                    errors.append(f"line {idx+1}: {cat} must come after {prev_cat}")
            prev_cat = cat
        prev_idx = idx

    # 3. within each category: sorted
    for cat in ['relative', 'thirdparty', 'std']:
        cat_entries = [(i, p) for i, p, c in ordered if c == cat]
        if len(cat_entries) < 2:
            continue
        sorted_entries = sorted(cat_entries, key=lambda x: x[1].lower())
        for (orig_i, orig_p), (_, sorted_p) in zip(cat_entries, sorted_entries):
            if orig_p != sorted_p:
                errors.append(f"line {orig_i+1}: '{orig_p}' out of order, expected: {[p for _,p in sorted_entries]}")
                break

    # 4. project includes: sub‑order client-server-shared-core
    proj = [(i, p) for i, p, c in ordered if c == 'project']
    def subcat(p):
        for idx, pref in enumerate(SRC_SUBFOLDERS):
            if p.startswith(pref):
                return idx
        return 999
    sorted_proj = sorted(proj, key=lambda x: (subcat(x[1]), x[1].lower()))
    for (orig_i, orig_p), (_, sorted_p) in zip(proj, sorted_proj):
        if orig_p != sorted_p:
            errors.append(f"line {orig_i+1}: project include '{orig_p}' sub‑order wrong; expected: {[p for _,p in sorted_proj]}")
            break

    # 5. max 2 consecutive empty lines
    for i in range(len(lines)-2):
        if lines[i].strip() == '' and lines[i+1].strip() == '' and lines[i+2].strip() == '':
            errors.append(f"line {i+1}: more than 2 consecutive empty lines")
            break
    return errors

@register_check("CMakeLists.txt referenced precisely once (tests/ and src/)")
def check_cmake_tree(ctx):
    root = 'CMakeLists.txt'
    if not os.path.isfile(root):
        return False, "root CMakeLists.txt missing"
    all_cmake = set()
    for d in ['tests', 'src']:
        if os.path.isdir(d):
            for path in glob_recursive(d, 'CMakeLists.txt'):
                rel = os.path.relpath(path, '.')
                all_cmake.add(rel.replace('\\', '/'))
    visited = set()
    def traverse(cmake_path):
        if cmake_path in visited:
            return
        visited.add(cmake_path)
        dir_path = os.path.dirname(cmake_path) or '.'
        try:
            content = read_file(cmake_path)
        except FileNotFoundError:
            return
        for m in re.finditer(r'add_subdirectory\s*\(\s*([^\s)]+)', content):
            sub = m.group(1).strip('"')
            sub_path = os.path.normpath(os.path.join(dir_path, sub)).replace('\\', '/')
            sub_cmake = sub_path + '/CMakeLists.txt' if os.path.isdir(sub_path) else sub_path
            if os.path.isfile(sub_cmake):
                traverse(sub_cmake)
    traverse(root)
    missing = all_cmake - visited
    if missing:
        return False, f"not referenced: {', '.join(sorted(missing))}"
    return True, ""

@register_check("source files (.cpp/.hpp/.mesh etc.) mentioned exactly once")
def check_source_file_mentions(ctx):
    exts = SHADER_EXTS | CODE_EXTS
    all_files = []
    for d in ['tests', 'src']:
        if os.path.isdir(d):
            for f in glob_recursive(d, '*.*'):
                if os.path.splitext(f)[1] in exts:
                    rel = os.path.relpath(f, '.').replace('\\', '/')
                    all_files.append(rel)
    mentions = {}
    for cmake in glob_recursive('.', 'CMakeLists.txt'):
        rel_cmake = os.path.relpath(cmake, '.').replace('\\', '/')
        if not (rel_cmake.startswith('tests/') or rel_cmake.startswith('src/') or rel_cmake == 'CMakeLists.txt'):
            continue
        cmake_dir = os.path.dirname(cmake) or '.'
        try:
            content = read_file(cmake)
        except:
            continue
        tokens = re.findall(fr'[\w./-]+(?:{'|'.join(exts)})', content)
        for token in tokens:
            if '$' in token:
                continue
            resolved = os.path.normpath(os.path.join(cmake_dir, token)).replace('\\', '/')
            if os.path.isfile(resolved):
                rel_resolved = os.path.relpath(resolved, '.').replace('\\', '/')
                mentions.setdefault(rel_resolved, []).append(rel_cmake)
    errors = []
    for f in all_files:
        if f not in mentions:
            errors.append(f"{f} not mentioned")
        elif len(set(mentions[f])) > 1:
            errors.append(f"{f} mentioned in multiple: {mentions[f]}")
    if errors:
        return False, "; ".join(errors)
    return True, ""

@register_source_check(SourceSpec({'.h'}), "no .h files in tests/ or src/")
def check_no_h_files(ctx, fpath, content):
    return [f"found .h file: {fpath}"]

@register_source_check(HEADER_FILES, ".hpp files start with #pragma once or DONT_CHECK")
def check_hpp_start(ctx, fpath, content):
    first = strip_cpp_comments(content).lstrip().split('\n', 1)[0].strip()
    return [] if first in ('#pragma once', f'// DONT_CHECK {DONT_CHECKS['pragma_once']}') else [f"first line is: {first!r}"]

@register_source_check(CODE_FILES.binary(), ".hpp/.cpp files end with newline")
def check_newline_end(ctx, fpath, content: bytes):
    return ["missing trailing newline"] if not content.endswith(b'\n') else []

@register_source_check(CODE_FILES, "include order in .hpp/.cpp files")
def check_include_order_all(ctx, fpath, content):
    lines = content.splitlines(keepends=True)
    return _check_include_order(lines, fpath)

@register_source_check(CODE_FILES, "namespace closing comments match")
def check_namespace_comments(ctx, fpath, content):
    errors = []
    for ns in re.findall(r'namespace\s+([\w:]+)\s*\{', content):
        if not re.search(r'\}\s*//\s*namespace\s+' + re.escape(ns) + r'\s*$', content, re.MULTILINE):
            errors.append(f"missing closing comment for namespace {ns}")
    return errors

@register_source_check(CODE_FILES, f"line length <= {LINE_LENGTH_MAX}")
def check_line_length(ctx, fpath, content):
    lines = content.splitlines()
    if lines[0].startswith(f'// DONT_CHECK_ANY {DONT_CHECK['line_length']}'):
        return []
    return [f"line {i}: {len(l.rstrip())} chars" for i, l in enumerate(lines, 1) if len(l.rstrip()) > LINE_LENGTH_MAX]

@register_source_check(CODE_FILES, f"nesting depth <= {MAX_NESTING_DEPTH} (indent 4 spaces)")
def check_nesting_depth(ctx, fpath, content):
    errors = []
    for i, line in enumerate(content.splitlines(), 1):
        stripped = line.rstrip('\n')
        if not stripped: continue
        spaces = len(stripped) - len(stripped.lstrip(' '))
        if spaces // 4 > MAX_NESTING_DEPTH:
            errors.append(f"line {i}: depth {spaces // 4}")
    return errors

@register_source_check(CODE_FILES, "no forbidden words")
def check_no_typedef(ctx, fpath, content):
    cleaned = strip_cpp_comments(content)
    return content_find(cleaned, fr'\b{'|'.join(FORBIDDEN_WORDS)}\b', "contains forbidden words")

@register_source_check(CODE_FILES, f"numbers >={MAX_DIGITS_WITHOUT_SEPARATOR + 1} digits must have digit separators")
def check_digit_separators(ctx, fpath, content):
    lines = content.split('\n')
    if lines[0].startswith(f'// DONT_CHECK_ANY {DONT_CHECKS['digit_separators']}'):
        return []
    ignored_lines = set()
    for i in range(1, len(lines)):
        if f'// DONT_CHECK {DONT_CHECKS['digit_separators']}' in lines[i-1]:
            ignored_lines.add(i+1)

    errors = []
    state = 'normal'
    i = 0
    while i < len(content):
        c = content[i]
        if state == 'normal':
            if c == '/' and i+1 < len(content) and content[i+1] == '/':
                state = 'linecomment'
                i += 1
            elif c == '/' and i+1 < len(content) and content[i+1] == '*':
                state = 'blockcomment'
                i += 1
            elif c == '"':
                state = 'string'
            elif c.isdigit():
                start = i
                if c == '0' and i+1 < len(content) and content[i+1] in 'xXbB':
                    i += 2
                    while i < len(content) and (content[i].isdigit()
                           or content[i] in 'abcdefABCDEF' or content[i] == "'"):
                        i += 1
                    token = content[start:i]
                else:
                    i += 1
                    while i < len(content) and (content[i].isdigit() or content[i] == "'"):
                        i += 1
                    token = content[start:i]

                lineno = content[:start].count('\n') + 1
                if lineno not in ignored_lines:
                    if token.startswith(('0x','0X','0b','0B')):
                        digit_part = token[2:]
                    elif token.startswith('0') and len(token) > 1:
                        digit_part = token[1:]
                    else:
                        digit_part = token

                    digits_only = digit_part.replace("'", "")
                    if len(digits_only) > MAX_DIGITS_WITHOUT_SEPARATOR and "'" not in token:
                        errors.append(f"line {lineno}: {token} (missing digit separator)")
                continue   # i already advanced inside the block
        elif state == 'linecomment':
            if c == '\n':
                state = 'normal'
        elif state == 'blockcomment':
            if c == '*' and i+1 < len(content) and content[i+1] == '/':
                state = 'normal'
                i += 1
        elif state == 'string':
            if c == '\\':
                i += 1
            elif c == '"':
                state = 'normal'
        i += 1
    return errors
