import sys
import os
import re
import json
import subprocess
import glob
from pathlib import Path
from datetime import date
from typing import List, Tuple, Callable, Dict, Any

from script.colored_print import *
from script.checks_common import *

@register_file_check(FILES.VCPKG, "vcpkg.json version-string matches")
def check_vcpkg(ctx, content):
    data = json.loads(content)
    v = data.get('version-string')
    if v != ctx['version_str']:
        return False, f"expected '{ctx['version_str']}', got '{v}'"
    ctx['vcpkg_deps_count'] = len(data.get('dependencies', []))
    return True, ""

@register_file_check(FILES.ROOT_CMAKELISTS, "Root CMakeLists.txt VERSION matches")
def check_cmake_version(ctx, content):
    m = re.search(r'project\s*\([^)]*VERSION\s+(\d+\.\d+\.\d+)\s*[^)]*\)', content, re.IGNORECASE)
    if not m:
        m = re.search(r'set\s*\(\s*(?:PROJECT_)?VERSION\s+"?(\d+\.\d+\.\d+)"?\s*\)', content, re.IGNORECASE)
    if not m:
        return False, "VERSION not found in CMakeLists.txt"
    if m.group(1) != ctx['version_str']:
        return False, f"expected '{ctx['version_str']}', got '{m.group(1)}'"
    return True, ""

@register_file_check(FILES.README, "README.md contains version string")
def check_readme_version(ctx, content):
    pattern = re.escape(f"Version {ctx['major_name']} {ctx['version_str']} {ctx['minor_name']}.")
    if not re.search(pattern, content):
        return False, f"missing version line"
    return True, ""

@register_file_check(FILES.README, "README.md dependency count matches vcpkg.json")
def check_readme_deps(ctx, content):
    dep_section = re.search(r'Current dependencies:(.*?)(?=\n#|\Z)', content, re.DOTALL)
    if not dep_section:
        return False, "no 'Current dependencies:' section"
    items = re.findall(r'^\d+\.\s', dep_section.group(1), re.MULTILINE)
    dep_count = len(items)
    expected = ctx.get('vcpkg_deps_count', 0)
    if dep_count != expected:
        return False, f"README has {dep_count} deps, vcpkg.json has {expected}"
    return True, ""

@register_file_check(FILES.PROJECT_INFO, "ProjectInfo MAJOR_VERSION_NAME")
def check_projectinfo_major(ctx, content):
    m = re.search(r'MAJOR_VERSION_NAME\s*\{\s*"([^"]+)"\s*\};', content)
    if not m:
        return False, "MAJOR_VERSION_NAME not found"
    if m.group(1) != ctx['major_name']:
        return False, f"expected '{ctx['major_name']}', got '{m.group(1)}'"
    return True, ""

@register_file_check(FILES.PROJECT_INFO, "ProjectInfo MINOR_VERSION_NAME")
def check_projectinfo_minor(ctx, content):
    m = re.search(r'MINOR_VERSION_NAME\s*\{\s*"([^"]+)"\s*\};', content)
    if not m:
        return False, "MINOR_VERSION_NAME not found"
    if m.group(1) != ctx['minor_name']:
        return False, f"expected '{ctx['minor_name']}', got '{m.group(1)}'"
    return True, ""

@register_file_check(FILES.PROJECT_INFO, "ProjectInfo PROJECT_VERSION struct")
def check_projectinfo_version(ctx, content):
    pattern = r'PROJECT_VERSION\s*\{\s*\.epoch\s*=\s*(\d+)\s*,\s*\.major\s*=\s*(\d+)\s*,\s*\.minor\s*=\s*(\d+)\s*,\s*\.patch\s*=\s*(\d+)\s*,?\s*\};'
    m = re.search(pattern, content)
    if not m:
        return False, "PROJECT_VERSION struct not found"
    ep, maj, min_, patch = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    if (ep, maj, min_, patch) != (ctx['epoch'], ctx['major'], ctx['minor'], ctx['snapshot_index']):
        return False, f"expected epoch={ctx['epoch']} major={ctx['major']} minor={ctx['minor']} patch={ctx['snapshot_index']}, got epoch={ep} major={maj} minor={min_} patch={patch}"
    return True, ""

@register_check("version history folder exists")
def check_history_folder(ctx):
    folder = f"docs/version_history/{ctx['major_name']} {ctx['epoch']}.{ctx['major']}"
    if not os.path.isdir(folder):
        return False, f"folder '{folder}' not found"
    ctx['history_folder'] = folder
    return True, ""

@register_check("version history markdown file exists")
def check_history_file(ctx):
    folder = ctx.get('history_folder')
    if not folder:
        return False, "history folder not found (previous check failed)"
    fname = f"{ctx['major_name']} {ctx['epoch']}.{ctx['major']}.{ctx['minor']} {ctx['minor_name']}.md"
    path = os.path.join(folder, fname)
    if not os.path.isfile(path):
        return False, f"file '{path}' not found"
    ctx['history_file'] = path
    return True, ""

@register_file_check(FILES.HISTORY, "required headings '# Overview' and '# Snapshots' present")
def check_headings(ctx, content):
    has_overview = bool(re.search(r'^# Overview\s*$', content, re.MULTILINE))
    has_snapshots = bool(re.search(r'^# Snapshots\s*$', content, re.MULTILINE))
    if not has_overview or not has_snapshots:
        missing = []
        if not has_overview: missing.append('# Overview')
        if not has_snapshots: missing.append('# Snapshots')
        return False, f"missing headings: {', '.join(missing)}"
    return True, ""

@register_file_check(FILES.HISTORY, "Overview contains text")
def check_overview_text(ctx, content):
    m = re.search(r'^# Overview\s*$(.*?)(^# )', content, re.MULTILINE | re.DOTALL)
    if not m:
        m = re.search(r'^# Overview\s*$(.*)', content, re.MULTILINE | re.DOTALL)
    if m:
        text = m.group(1).strip()
        if text:
            return True, ""
    return False, "Overview section is empty"

def parse_snapshot_headings(content, major_name, version_str):
    sec = re.search(r'^# Snapshots\s*$(.*?)(\Z|^# )', content, re.MULTILINE | re.DOTALL)
    if not sec:
        return []
    snap_text = sec.group(1)
    pattern = re.compile(
        r'^#{1,2} ' + re.escape(major_name) + r' ' + re.escape(version_str) + r':(\d+)\((\d{2}\.\d{2}\.\d{2})\)\s*$',
        re.MULTILINE
    )
    results = []
    for m in pattern.finditer(snap_text):
        idx = int(m.group(1))
        date_str = m.group(2)
        results.append((idx, date_str))
    return results

@register_file_check(FILES.HISTORY, "snapshots 1..SnapshotIndex exist with correct format")
def check_snapshot_indices(ctx, content):
    snapshots = parse_snapshot_headings(content, ctx['major_name'], ctx['version_str'])
    existing = {idx for idx, _ in snapshots}
    required = set(range(1, ctx['snapshot_index']+1))
    missing = required - existing
    if missing:
        return False, f"missing snapshots: {sorted(missing)}"
    ctx['_snapshots'] = snapshots
    return True, ""

@register_file_check(FILES.HISTORY, "snapshot sections are not empty")
def check_snapshot_nonempty(ctx, content):
    snapshots = ctx.get('_snapshots', [])
    if not snapshots:
        return False, "no snapshots found"
    major_name = ctx['major_name']
    version_str = ctx['version_str']
    for idx, date_str in snapshots:
        pattern = rf'^#{1,2} {re.escape(major_name)} {re.escape(version_str)}:{idx}\({date_str}\)\s*$(.*?)(?=^#{1,2} {re.escape(major_name)}|\Z)'
        m = re.search(pattern, content, re.MULTILINE | re.DOTALL)
        if m:
            section_content = m.group(1).strip()
            if not section_content:
                return False, f"snapshot {idx} is empty"
        else:
            return False, f"snapshot {idx} section not found"
    return True, ""

@register_check("snapshot dates are non-decreasing")
def check_dates_order(ctx):
    snapshots = ctx.get('_snapshots', [])
    if not snapshots:
        return False, "no snapshots found"
    prev = None
    for idx, date_str in sorted(snapshots):
        if prev and date_str < prev:
            return False, f"date {date_str} at snapshot {idx} is earlier than previous {prev}"
        prev = date_str
    return True, ""

@register_check("latest snapshot date is today")
def check_today_date(ctx):
    snapshots = ctx.get('_snapshots', [])
    if not snapshots:
        return False, "no snapshots found"
    latest = max(snapshots, key=lambda x: x[0])
    if latest[0] != ctx['snapshot_index']:
        return False, f"latest snapshot index {latest[0]} != {ctx['snapshot_index']}"
    today = date.today().strftime("%y.%m.%d")
    if latest[1] != today:
        return False, f"snapshot date is {latest[1]}, but today is {today}"
    return True, ""
