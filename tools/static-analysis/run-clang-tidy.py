#!/usr/bin/env python3
"""Run clang-tidy on TDLua-owned production translation units only."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
HEADER_FILTER = r"(^|.*/)(src|include/tdlua)/.*"


def inside(path, parent):
    try:
        return os.path.commonpath((str(path), str(parent))) == str(parent)
    except ValueError:
        return False


def cache_value(cache, name):
    prefix = name + ":"
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[1]
    return ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="one CMake build directory for exactly one backend/Lua ABI")
    parser.add_argument("--expect-backend", choices=("json", "native"), required=True)
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--report", type=Path,
                        help="report file (default: <build-dir>/clang-tidy-report.txt)")
    args = parser.parse_args()

    executable = shutil.which(args.clang_tidy)
    if not executable:
        parser.error("clang-tidy executable not found: " + args.clang_tidy)

    build = args.build_dir.resolve()
    cache = build / "CMakeCache.txt"
    database = build / "compile_commands.json"
    if not cache.is_file() or not database.is_file():
        parser.error("build directory must contain CMakeCache.txt and compile_commands.json")
    if Path(cache_value(cache, "CMAKE_HOME_DIRECTORY")).resolve() != ROOT:
        parser.error("build directory belongs to a different source checkout")

    backend = cache_value(cache, "TDLUA_BACKEND") or "json"
    lua_version = cache_value(cache, "TDLUA_LUA_VERSION")
    lua_library = cache_value(cache, "LUA_LIBRARY") or cache_value(cache, "LUA_LIBRARIES")
    if not lua_version:
        library_version = re.search(r"lib(?:lua|luajit)[-]?([0-9]+\.[0-9]+)", lua_library)
        if library_version:
            lua_version = library_version.group(1)
            if "luajit" in lua_library.lower():
                lua_version = "LuaJIT ABI " + lua_version
    lua_version = lua_version or "detected from compilation commands"
    if backend != args.expect_backend:
        parser.error("expected backend %s, build directory is %s" % (args.expect_backend, backend))

    entries = json.loads(database.read_text(encoding="utf-8"))
    selected = []
    generated_include_dirs = set()
    for entry in entries:
        source = Path(entry["file"])
        if not source.is_absolute():
            source = Path(entry["directory"]) / source
        source = source.resolve()
        if not inside(source, ROOT / "src"):
            continue
        if not source.is_file():
            parser.error("source from compilation database is missing: " + str(source))
        selected.append(source)
        command = entry.get("command", "")
        if not command and "arguments" in entry:
            command = " ".join(entry["arguments"])
        for token in re.findall(r"(?:^|\s)-I([^\s]+)", command):
            include_dir = Path(token.strip('"'))
            if not include_dir.is_absolute():
                include_dir = Path(entry["directory"]) / include_dir
            if "generated" in include_dir.parts:
                generated_include_dirs.add(include_dir.resolve())

    selected = sorted(set(selected))
    if not selected:
        parser.error("no TDLua production sources under src/ were found in the database")
    if backend == "native":
        relevant = [path for path in generated_include_dirs if "tdlua" in path.parts]
        if not relevant or any(not path.is_dir() for path in relevant):
            parser.error("native generated include directory is absent; build target tdlua first")

    version = subprocess.run([executable, "--version"], check=True, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()
    report = args.report.resolve() if args.report else build / "clang-tidy-report.txt"
    report.parent.mkdir(parents=True, exist_ok=True)
    command = [executable, "-p", str(build), "--config-file", str(ROOT / ".clang-tidy"),
               "--header-filter", HEADER_FILTER] + [str(path) for path in selected]
    result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    warning_count = len(re.findall(r"\bwarning:\s", result.stdout))
    error_count = len(re.findall(r"\berror:\s", result.stdout))
    header = [
        "clang-tidy diagnostic report",
        "source: " + str(ROOT),
        "build: " + str(build),
        "backend: " + backend,
        "Lua ABI: " + lua_version,
        "tool: " + version.replace("\n", " "),
        "translation units: %d (only source files under src/)" % len(selected),
        "generated include directories checked: %d" % len(generated_include_dirs),
        "warnings: %d" % warning_count,
        "errors: %d" % error_count,
        "clang-tidy exit: %d" % result.returncode,
        "",
    ]
    report.write_text("\n".join(header) + result.stdout, encoding="utf-8")
    print("clang-tidy analyzed %d TDLua sources (%s, Lua %s)" %
          (len(selected), backend, lua_version))
    print("diagnostics: %d warnings, %d errors; report: %s" %
          (warning_count, error_count, report))
    if result.returncode:
        print(result.stdout, file=sys.stderr)
        return result.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
