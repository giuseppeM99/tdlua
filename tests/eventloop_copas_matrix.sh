#!/usr/bin/env bash
# Copyright (c) 2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause
#
# Run the Copas P1 suites on every available interpreter.
#
# Environment:
#   TDLUA_ROOT        repository/worktree root (default: parent of tests/)
#   COPAS_ENV         LuaRocks trees with Copas 4.12.0
#                     (default: ../tdlua-v0.6/E2-COPAS-P0/env)
#   EVIDENCE_DIR      log directory (default: ./.copas-evidence)
#   TDLIB_LIB         TDLib shared library directory
#
# The script does not install anything and does not modify the source tree.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COPAS_ENV="${COPAS_ENV:-$ROOT/../tdlua-v0.6/E2-COPAS-P0/env}"
EVIDENCE_DIR="${EVIDENCE_DIR:-$ROOT/.copas-evidence}"
TDLIB_LIB="${TDLIB_LIB:-/tmp/opencode/tdlib-1.8.70/lib}"
TDLUA_55_MODULE="${TDLUA_55_MODULE:-/home/doctor/Projects/tdlua/build}"
TDLUA_55_LIB="${TDLUA_55_LIB:-/home/doctor/Projects/tdlua/build/td}"
mkdir -p "$EVIDENCE_DIR"
: > "$EVIDENCE_DIR/summary.tsv"

declare -A BIN=(
  [5.1]=lua5.1 [5.2]=lua5.2 [5.3]=lua5.3 [5.4]=lua5.4 [5.5]=lua5.5
  [luajit]=luajit [luajit-off]="luajit -joff"
)
declare -A ROCKS=(
  [5.1]=5.1 [5.2]=5.2 [5.3]=5.3 [5.4]=5.4 [5.5]=5.5
  [luajit]=5.1 [luajit-off]=5.1
)
declare -A MODULE=(
  [5.1]=/tmp/opencode/build-json-5.1
  [5.2]=/tmp/opencode/build-json-5.2
  [5.3]=/tmp/opencode/build-json-5.3
  [5.4]=/tmp/opencode/build-json-54
  [5.5]=$TDLUA_55_MODULE
  [luajit]=/tmp/opencode/build-json-luajit
  [luajit-off]=/tmp/opencode/build-json-luajit
)

run_one() {
  local runtime="$1" kind="$2" script="$3" extra="$4"
  local bin="${BIN[$runtime]}" rocks="${ROCKS[$runtime]}"
  local mod="${MODULE[$runtime]}"
  local log="$EVIDENCE_DIR/${runtime}-${kind}-${script}.log"

  if ! eval "command -v ${bin%% *}" >/dev/null 2>&1; then
    printf 'SKIP\t%s\t%s\t%s\tmissing interpreter\n' \
      "$runtime" "$kind" "$script" >> "$EVIDENCE_DIR/summary.tsv"
    return
  fi
  if [ ! -f "$mod/tdlua.so" ] &&
     { [ "$kind" = "runtime" ] || [ "$kind" = "negative" ]; }; then
    printf 'SKIP\t%s\t%s\t%s\tmissing %s/tdlua.so\n' \
      "$runtime" "$kind" "$script" "$mod" >> "$EVIDENCE_DIR/summary.tsv"
    return
  fi
  if [ ! -d "$COPAS_ENV/rocks-$rocks" ]; then
    printf 'SKIP\t%s\t%s\t%s\tmissing %s/rocks-%s\n' \
      "$runtime" "$kind" "$script" "$COPAS_ENV" "$rocks" \
      >> "$EVIDENCE_DIR/summary.tsv"
    return
  fi

  eval "$(luarocks --lua-version="$rocks" --tree="$COPAS_ENV/rocks-$rocks" path)"
  LUA_CPATH="$mod/?.so;$LUA_CPATH" \
  LD_LIBRARY_PATH="$TDLIB_LIB:$TDLUA_55_LIB:${LD_LIBRARY_PATH:-}" \
  LUA_PATH="$ROOT/?.lua;$ROOT/?/init.lua;$LUA_PATH" \
    timeout 300 $bin "$ROOT/tests/$script" $extra > "$log" 2>&1
  local rc=$?
  local line
  line=$(grep '^SUMMARY' "$log" | tail -1)
  printf '%s\t%s\t%s\t%s\t%s\n' "$rc" "$runtime" "$kind" "$script" \
    "${line:-no summary}" >> "$EVIDENCE_DIR/summary.tsv"
}

RUNTIMES=(5.1 5.2 5.3 5.4 5.5 luajit luajit-off)

for runtime in "${RUNTIMES[@]}"; do
  echo "== $runtime =="
  run_one "$runtime" fake eventloop_copas_test.lua ""
  run_one "$runtime" fake eventloop_copas_concurrent.lua ""
  run_one "$runtime" fake eventloop_copas_reconnect.lua ""
  run_one "$runtime" fake eventloop_copas_unreliable.lua ""
  run_one "$runtime" runtime eventloop_copas_runtime.lua ""
  run_one "$runtime" negative eventloop_copas_negative.lua "direct"
  run_one "$runtime" negative eventloop_copas_negative.lua "supplied"
  run_one "$runtime" negative eventloop_copas_negative.lua "await"
done

echo
column -t -s $'\t' "$EVIDENCE_DIR/summary.tsv" 2>/dev/null ||
  cat "$EVIDENCE_DIR/summary.tsv"

failures=$(awk -F'\t' '$1 != 0 && $1 != "SKIP" {count++} END {print count+0}' \
  "$EVIDENCE_DIR/summary.tsv")
if [ "$failures" -ne 0 ]; then
  echo "FAILED: $failures run(s)" >&2
  exit 1
fi
echo "Copas P1 suites: no failures"
