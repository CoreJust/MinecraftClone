#!/usr/bin/env python3
"""
Cross-platform version publishing script with color output.
Usage:
    python publish_version.py "<MajorName>:<MinorName>" <Epoch>.<Major>.<Minor>:<SnapshotIndex> [--checks-only]
"""

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
import script.infrastructure_checks
import script.code_checks

# ----- Argument parsing -----
def parse_args():
    if len(sys.argv) < 3:
        print_help()
        sys.exit(1)

    checks_only = False
    args = sys.argv[1:]
    if "--checks-only" in args:
        checks_only = True
        args.remove("--checks-only")

    if len(args) != 2:
        print_help()
        sys.exit(1)

    names_part, version_part = args

    names_part = names_part.strip('"')
    if ':' not in names_part:
        print_fail("Names argument must be in format 'MajorName:MinorName'")
        print_help()
        sys.exit(1)
    major_name, minor_name = names_part.split(':', 1)

    version_re = re.compile(r'^(\d+)\.(\d+)\.(\d+):(\d+)$')
    m = version_re.match(version_part)
    if not m:
        print_fail("Version argument must be in format Epoch.Major.Minor:SnapshotIndex")
        print_help()
        sys.exit(1)
    epoch, major, minor, snapshot_index = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    version_str = f"{epoch}.{major}.{minor}"

    return major_name, minor_name, epoch, major, minor, version_str, snapshot_index, checks_only

def print_help():
    print(Color.colorize("Usage:", Color.YELLOW))
    print("  python publish_version.py \"<MajorName>:<MinorName>\" <Epoch>.<Major>.<Minor>:<SnapshotIndex> [--checks-only]")
    print(Color.colorize("Example:", Color.GRAY))
    print("  python publish_version.py \"Crimson:Dawn\" 1.2.3:4")
    print("  --checks-only    Run only the common checks, skip tests and publishing.")

@register_check("no unstaged or uncommitted changes")
def check_git_clean(ctx):
    try:
        status = git_output('status', '--porcelain')
        if status:
            return False, "working directory is not clean"
        return True, ""
    except RuntimeError as e:
        return False, str(e)

# ----- Tests -----
def run_tests():
    print_section("\n--- Running tests ---")
    print_info("Building debug preset...")
    build = subprocess.run(['cmake', '--build', '--preset', 'debug'], capture_output=True, text=True)
    if build.returncode != 0:
        print_fail("BUILD FAILED", prefix="FAIL")
        print(build.stderr)
        print(build.stdout)
        return False
    print_pass("Build successful")
    print_info("Running tests...")
    test = subprocess.run(['ctest', '--preset', 'debug', '--output-on-failure'])
    if test.returncode != 0:
        print_fail("Test run contains failures", prefix="FAIL")
        return False
    print_pass("All tests passed")
    return True

# ----- Publish -----
def publish(ctx):
    print_section("\n--- Publishing ---")
    try:
        branch = git_output('rev-parse', '--abbrev-ref', 'HEAD')
    except RuntimeError as e:
        print_fail(str(e))
        return False
    if branch != 'dev':
        print_fail(f"Must be on 'dev' branch, currently on '{branch}'")
        return False

    today_str = date.today().strftime('%y.%m.%d')
    msg = f"{ctx['major_name']} {ctx['version_str']}:{ctx['snapshot_index']}({today_str})"
    merge_cmd = ['git', 'merge', '--no-ff', 'dev', '-m', msg]
    print_info(f"Merging: {' '.join(merge_cmd)}")
    merge = subprocess.run(merge_cmd)
    if merge.returncode != 0:
        print_fail("Merge failed")
        return False

    tag_name = f"{ctx['major_name']}/{ctx['version_str']}/{ctx['snapshot_index']}_{today_str}"
    tag_msg = f"{ctx['major_name']} {ctx['version_str']} {ctx['minor_name']} snapshot {ctx['snapshot_index']}({today_str})"
    tag_cmd = ['git', 'tag', '-a', tag_name, '-m', tag_msg]
    print_info(f"Tagging: {' '.join(tag_cmd)}")
    tag = subprocess.run(tag_cmd)
    if tag.returncode != 0:
        print_fail("Tagging failed")
        return False

    push_cmd = ['git', 'push', '--follow-tags']
    print_info(f"Pushing: {' '.join(push_cmd)}")
    push = subprocess.run(push_cmd)
    if push.returncode != 0:
        print_fail("Push failed")
        return False
    print_pass("Publishing completed")
    return True

def main():
    major_name, minor_name, epoch, major, minor, version_str, snapshot_index, checks_only = parse_args()

    ctx = {
        'major_name': major_name,
        'minor_name': minor_name,
        'epoch': epoch,
        'major': major,
        'minor': minor,
        'version_str': version_str,
        'snapshot_index': snapshot_index,
    }

    print_section("=== Common Checks ===")
    all_ok = run_checks(ctx)
    if not all_ok:
        print_fail("Some checks failed. Aborting.", prefix="ERROR")
        sys.exit(1)

    if checks_only:
        print_warning("--checks-only specified, stopping after checks.")
        return

    if not run_tests():
        print_fail("Tests failed. Aborting.", prefix="ERROR")
        sys.exit(1)

    if not publish(ctx):
        print_fail("Publishing failed.", prefix="ERROR")
        sys.exit(1)

if __name__ == '__main__':
    main()
