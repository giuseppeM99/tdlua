#!/usr/bin/env python3
"""Check selected or changed TDLua C/C++ files without rewriting them."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
EXTENSIONS = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}


def inside(path, parent):
    try:
        return os.path.commonpath((str(path), str(parent))) == str(parent)
    except ValueError:
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", help="files to check")
    parser.add_argument("--changed-from", help="also check files changed from a Git ref")
    parser.add_argument("--clang-format", default="clang-format")
    parser.add_argument("--diagnostic", action="store_true",
                        help="report style differences without failing on them")
    parser.add_argument("--report", type=Path, help="write a plain-text report")
    args = parser.parse_args()

    executable = shutil.which(args.clang_format)
    if not executable:
        parser.error("clang-format executable not found: " + args.clang_format)
    names = list(args.files)
    if args.changed_from:
        changed = subprocess.run(
            ["git", "diff", "--name-only", "--diff-filter=ACMRT", args.changed_from, "--"],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if changed.returncode:
            parser.error("cannot compare Git ref %s: %s" %
                         (args.changed_from, changed.stderr.strip()))
        names.extend(changed.stdout.splitlines())
    if not names and not args.changed_from:
        parser.error("pass files or --changed-from REF")

    selected = []
    for name in sorted(set(names)):
        path = (ROOT / name).resolve()
        if path.suffix not in EXTENSIONS:
            continue
        if not (inside(path, ROOT / "src") or inside(path, ROOT / "include" / "tdlua")):
            continue
        if path.is_file():
            selected.append(path)

    version = subprocess.run([executable, "--version"], check=True, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()
    lines = ["clang-format report", "tool: " + version.replace("\n", " "),
             "files checked: %d" % len(selected)]
    violations = []
    for path in selected:
        result = subprocess.run([executable, "--output-replacements-xml", str(path)],
                                cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            return result.returncode
        if "<replacement " in result.stdout:
            violations.append(path.relative_to(ROOT).as_posix())
    lines.append("files with formatting differences: %d" % len(violations))
    lines.extend(violations)
    report_text = "\n".join(lines) + "\n"
    if args.report:
        report = args.report.resolve()
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(report_text, encoding="utf-8")
    print(report_text, end="")
    if violations and not args.diagnostic:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
