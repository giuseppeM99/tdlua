#!/usr/bin/env python3
# Copyright (c) 2018-2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause
"""Generate a versioned LuaRocks release rockspec from an SCM rockspec.

`dist.info` is the single source of truth for the release version. LuaRocks
only accepts a rockspec whose filename is `<package>-<version>-<revision>.rockspec`,
so the generated file is named from the package and version it declares rather
than from the git tag.

The release rockspec points at the published source tarball instead of the git
repository: the tarball is the only artifact that carries the pinned `td` and
`base64` submodules that a bundled TDLib build needs.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

DIST_VERSION = re.compile(r'^version\s*=\s*"([^"]+)"\s*$', re.MULTILINE)
PACKAGE = re.compile(r'^package\s*=\s*"([^"]+)"\s*$', re.MULTILINE)
SCM_VERSION = re.compile(r'^version\s*=\s*"scm-1"\s*$', re.MULTILINE)
SOURCE_BLOCK = re.compile(r'^source\s*=\s*\{.*?^\}', re.MULTILINE | re.DOTALL)


def read_base_version(dist_info: Path) -> str:
    match = DIST_VERSION.search(dist_info.read_text(encoding="utf-8"))
    if not match:
        raise SystemExit(f"cannot read the release version from {dist_info}")
    return match.group(1)


def source_url(tag: str) -> str:
    return (
        "https://github.com/giuseppeM99/tdlua/releases/download/"
        f"{tag}/tdlua-{tag}-source.tar.gz"
    )


def generate(text: str, base_version: str, url: str) -> tuple[str, str]:
    package_match = PACKAGE.search(text)
    if not package_match:
        raise SystemExit("the rockspec does not declare a package name")
    package = package_match.group(1)

    version = f"{base_version}-1"
    text = SCM_VERSION.sub(f'version = "{version}"', text, count=1)
    if f'version = "{version}"' not in text:
        raise SystemExit('the rockspec does not declare version = "scm-1"')

    replacement = 'source = {\n   url = "%s"\n}' % url
    text, replacements = SOURCE_BLOCK.subn(replacement, text, count=1)
    if replacements != 1:
        raise SystemExit("the rockspec does not declare a source block")

    return package, text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rockspec", required=True, type=Path,
                        help="SCM rockspec to derive the release rockspec from")
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="directory that receives the generated rockspec")
    parser.add_argument("--dist-info", type=Path,
                        default=REPO_ROOT / "dist.info",
                        help="release metadata that carries the base version")
    parser.add_argument("--base-version",
                        help="override the base version read from dist.info")
    parser.add_argument("--tag",
                        help="release tag that hosts the source tarball "
                             "(default: v<base-version>)")
    args = parser.parse_args()

    base_version = args.base_version or read_base_version(args.dist_info)
    tag = args.tag or f"v{base_version}"
    url = source_url(tag)

    package, text = generate(args.rockspec.read_text(encoding="utf-8"),
                             base_version, url)

    filename = f"{package}-{base_version}-1.rockspec"
    expected = re.compile(r"^[A-Za-z0-9_.+-]+-\d+\.\d+\.\d+-\d+\.rockspec$")
    if not expected.match(filename):
        raise SystemExit(f"generated filename is not a LuaRocks rockspec name: {filename}")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir / filename
    output.write_text(text, encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
