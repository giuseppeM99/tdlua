#!/usr/bin/env bash
# Copyright (c) 2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause
#
# CI-only Copas interoperability matrix.
#
# Environment (all required):
#   COPAS_ENV         directory containing rocks-5.1 ... rocks-5.5
#   TDLUA_BUILD_ROOT  directory containing build-copas-<runtime>/tdlua.so
#   LUA55_BIN         optional; defaults to lua5.5 in PATH
#   EVIDENCE_DIR      optional; defaults to .ci/copas-evidence
#
# Runtimes: 5.1 5.2 5.3 5.4 5.5 luajit luajit-off (11 runs each, 77 rows).
# The script installs nothing and fails closed: a missing module, a skip, a
# non-zero exit, a wrong row count or a wrong assertion count is an error.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COPAS_ENV="${COPAS_ENV:-}"
TDLUA_BUILD_ROOT="${TDLUA_BUILD_ROOT:-$ROOT}"
EVIDENCE_DIR="${EVIDENCE_DIR:-$ROOT/.ci/copas-evidence}"
LUA55_BIN="${LUA55_BIN:-lua5.5}"

if [ -z "$COPAS_ENV" ]; then
  echo "::error::COPAS_ENV is required" >&2
  exit 2
fi
if [ ! -d "$COPAS_ENV" ]; then
  echo "::error::COPAS_ENV does not exist: $COPAS_ENV" >&2
  exit 2
fi

mkdir -p "$EVIDENCE_DIR"
# Stale logs from a previous run must not satisfy the skip/FAIL greps when this
# run produced fewer cases.
rm -f "$EVIDENCE_DIR"/*.log
: > "$EVIDENCE_DIR/summary.tsv"
: > "$EVIDENCE_DIR/environment.tsv"

RUNTIMES=(5.1 5.2 5.3 5.4 5.5 luajit luajit-off)
EXPECTED_ROWS=77
EXPECTED_CHECKS=3239

binary_for() {
  case "$1" in
    5.1|5.2|5.3|5.4) echo "lua$1" ;;
    5.5) echo "$LUA55_BIN" ;;
    luajit|luajit-off) echo "luajit" ;;
    *) return 1 ;;
  esac
}

lua_dir_for() {
  case "$1" in
    luajit|luajit-off) echo "5.1" ;;
    *) echo "$1" ;;
  esac
}

build_for() {
  case "$1" in
    luajit|luajit-off) echo "luajit" ;;
    *) echo "$1" ;;
  esac
}

fail() {
  echo "::error::$*" >&2
  exit 1
}

run_case() {
  local runtime="$1" kind="$2" script="$3" extra="${4:-}"
  local rv build module tree log rc line
  rv="$(lua_dir_for "$runtime")"
  build="$(build_for "$runtime")"
  module="$TDLUA_BUILD_ROOT/build-copas-$build"
  tree="$COPAS_ENV/rocks-$rv"
  local mode_tag="${extra:+$extra-}"
  log="$EVIDENCE_DIR/${runtime}-${kind}-${mode_tag}${script}.log"

  [ -f "$module/tdlua.so" ] ||
    fail "missing compiled module $module/tdlua.so for $runtime"
  [ -d "$tree" ] || fail "missing rocks tree $tree for $runtime"
  [ -f "$ROOT/tests/$script" ] || fail "missing test $ROOT/tests/$script"

  local -a cmd
  if [ "$runtime" = "luajit-off" ]; then
    cmd=(luajit -joff)
  else
    cmd=("$(binary_for "$runtime")")
  fi
  if [ -n "$extra" ]; then
    cmd+=("$ROOT/tests/$script" "$extra")
  else
    cmd+=("$ROOT/tests/$script")
  fi

  local timeout_s=300
  case "$kind" in
    negative) timeout_s=120 ;;
  esac

  env \
    LUA_PATH="$tree/share/lua/$rv/?.lua;$tree/share/lua/$rv/?/init.lua;$ROOT/?.lua;$ROOT/?/init.lua;;" \
    LUA_CPATH="$module/?.so;$tree/lib/lua/$rv/?.so;;" \
    timeout "$timeout_s" "${cmd[@]}" >"$log" 2>&1
  rc=$?
  line="$(grep '^SUMMARY' "$log" | tail -1)"
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$rc" "$runtime" "$kind" "$script" "${line:-no summary}" "$log" \
    >>"$EVIDENCE_DIR/summary.tsv"
  if [ "$rc" -ne 0 ]; then
    echo "::error::$runtime $kind $script exited with $rc" >&2
    tail -20 "$log" >&2
  fi
}

for runtime in "${RUNTIMES[@]}"; do
  echo "== $runtime =="
  rv="$(lua_dir_for "$runtime")"
  build="$(build_for "$runtime")"
  bin="$(binary_for "$runtime")"
  command -v "$bin" >/dev/null 2>&1 || fail "missing interpreter $bin"
  module="$TDLUA_BUILD_ROOT/build-copas-$build"
  tree="$COPAS_ENV/rocks-$rv"
  [ -f "$module/tdlua.so" ] || fail "missing module for $runtime"

  preflight="$EVIDENCE_DIR/${runtime}-preflight.log"
  preflight_script="$EVIDENCE_DIR/${runtime}-preflight.lua"
  cat >"$preflight_script" <<'LUA'
local function searchpath(name, path)
  if package.searchpath then return package.searchpath(name, path) end
  local module = name:gsub("%.", "/")
  for template in path:gmatch("[^;]+") do
    local file = template:gsub("%?", module)
    local handle = io.open(file, "r")
    if handle then handle:close(); return file end
  end
  return nil
end
local path = searchpath("tdlua", package.cpath)
assert(path ~= nil, "tdlua module is not on package.cpath")
local tdlua = require("tdlua")
assert(type(tdlua.capabilities) == "table", "missing capabilities table")
assert(tdlua.capabilities.on_complete == true,
  "missing capabilities.on_complete")
assert(tdlua.capabilities.managed_observers == nil,
  "unexpected capabilities.managed_observers")
local socket = require("socket")
local client = tdlua()
local future = assert(client:getOption({name = "version"}))
local deadline = socket.gettime() + 20
while not future:ready() and socket.gettime() < deadline do
  client:receive(0.01)
end
assert(future:ready(), "TDLib worker did not answer getOption")
local response = future:wait()
local value = type(response) == "table" and (response.text or response.value)
assert(value ~= nil, "empty getOption result")
local copas = require("copas")
client:close()
local closed = socket.gettime() + 5
while not client:isClosed() and socket.gettime() < closed do
  client:receive(0.01)
end
print(string.format("%s\t%s\t%s\t%s\t%s",
  path, tostring(value), socket._VERSION, copas._VERSION or "4.12.0",
  "on_complete"))
LUA
  pre_cmd=()
  if [ "$runtime" = "luajit-off" ]; then
    pre_cmd=(luajit -joff)
  else
    pre_cmd=("$bin")
  fi
  env \
    LUA_PATH="$tree/share/lua/$rv/?.lua;$tree/share/lua/$rv/?/init.lua;$ROOT/?.lua;$ROOT/?/init.lua;;" \
    LUA_CPATH="$module/?.so;$tree/lib/lua/$rv/?.so;;" \
    timeout 60 "${pre_cmd[@]}" "$preflight_script" >"$preflight" 2>&1 ||
    fail "preflight failed for $runtime (see $preflight)"
  printf '%s\t%s\n' "$runtime" "$(tail -1 "$preflight")" \
    >>"$EVIDENCE_DIR/environment.tsv"

  run_case "$runtime" deterministic eventloop_copas_test.lua
  run_case "$runtime" deterministic eventloop_copas_concurrent.lua
  run_case "$runtime" deterministic eventloop_copas_reconnect.lua
  run_case "$runtime" deterministic eventloop_copas_unreliable.lua
  run_case "$runtime" deterministic eventloop_copas_completion_test.lua
  run_case "$runtime" deterministic eventloop_copas_completion_fallback.lua
  run_case "$runtime" runtime eventloop_copas_runtime.lua
  run_case "$runtime" runtime eventloop_copas_completion_runtime.lua
  run_case "$runtime" negative eventloop_copas_negative.lua direct
  run_case "$runtime" negative eventloop_copas_negative.lua supplied
  run_case "$runtime" negative eventloop_copas_negative.lua await
done

rows="$(wc -l <"$EVIDENCE_DIR/summary.tsv")"
[ "$rows" -eq "$EXPECTED_ROWS" ] ||
  fail "expected $EXPECTED_ROWS matrix rows, got $rows"

for runtime in "${RUNTIMES[@]}"; do
  n="$(awk -F'\t' -v r="$runtime" '$2 == r' "$EVIDENCE_DIR/summary.tsv" |
    wc -l)"
  [ "$n" -eq 11 ] || fail "expected 11 rows for $runtime, got $n"
done

non_zero="$(awk -F'\t' '$1 != 0' "$EVIDENCE_DIR/summary.tsv")"
[ -z "$non_zero" ] || fail "non-zero suite exits:
$non_zero"

missing="$(awk -F'\t' '$5 == "no summary"' "$EVIDENCE_DIR/summary.tsv")"
[ -z "$missing" ] || fail "suite did not print SUMMARY:
$missing"

# A missing or malformed `failures=` field must not be read as zero by the
# aggregators below. Require both counters on every row.
bad_summary="$(awk -F'\t' \
  '$5 !~ /^SUMMARY [^[:space:]].* checks=[0-9][0-9]* failures=[0-9][0-9]*([[:space:]]|$)/ {print}' \
  "$EVIDENCE_DIR/summary.tsv")"
[ -z "$bad_summary" ] || fail "malformed SUMMARY line:
$bad_summary"

skips="$(grep -hE '^[[:space:]]*skip' "$EVIDENCE_DIR"/*.log || true)"
[ -z "$skips" ] || fail "suite reported a skip:
$skips"

fails="$(grep -hE '^[[:space:]]*FAIL|^[[:space:]]*not ok' \
  "$EVIDENCE_DIR"/*.log || true)"
[ -z "$fails" ] || fail "suite reported a failed check:
$fails"

total_checks="$(awk -F'checks=' '{if (NF > 1) {split($2, a, " "); s += a[1]}} END {print s + 0}' \
  "$EVIDENCE_DIR/summary.tsv")"
[ "$total_checks" -eq "$EXPECTED_CHECKS" ] ||
  fail "expected $EXPECTED_CHECKS checks, got $total_checks"

total_failures="$(awk -F'failures=' '{if (NF > 1) {split($2, a, " "); s += a[1]}} END {print s + 0}' \
  "$EVIDENCE_DIR/summary.tsv")"
[ "$total_failures" -eq 0 ] ||
  fail "matrix reported $total_failures failed assertions"

echo
echo "runtime / module / TDLib / LuaSocket / Copas"
if command -v column >/dev/null 2>&1; then
  column -t -s $'\t' "$EVIDENCE_DIR/environment.tsv"
else
  cat "$EVIDENCE_DIR/environment.tsv"
fi
echo
echo "rows / checks / failures"
printf '%s / %s / %s\n' "$rows" "$total_checks" "$total_failures"
echo "Copas CI matrix: PASS"
