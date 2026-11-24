#!/bin/bash
# Negative fixtures for tools/sync-classifier/generate_sync_classifier.cmake.
#
# Each case copies the pinned TDLib sources, applies one change that alters the
# classification without changing the case names, and requires the generator to
# fail. A generator that accepted it would emit the wrong method set.
set -u
repo=/home/doctor/Projects/tdlua-v063-sync
gen="$repo/tools/sync-classifier/generate_sync_classifier.cmake"
work=/tmp/opencode/classifier-fixtures
failures=0
cases=0

prepare() {
  rm -rf "$work/$1"
  mkdir -p "$work/$1/td/telegram"
  cp "$repo/td/td/telegram/SynchronousRequests.cpp" "$work/$1/td/telegram/"
  cp "$repo/td/td/telegram/SynchronousRequests.h" "$work/$1/td/telegram/"
  cp "$repo/td/td/telegram/OptionManager.cpp" "$work/$1/td/telegram/"
  out="$work/$1/out"
}

expect_failure() {
  local name=$1 description=$2
  cases=$((cases + 1))
  if cmake -DTDLIB_SOURCE_DIR="$work/$name" -DTDLIB_VERSION=1.8.71 \
       -DOUTPUT_DIR="$out" -P "$gen" >"$work/$name.log" 2>&1; then
    echo "FAIL  $name: generator accepted $description"
    failures=$((failures + 1))
  else
    echo "ok    $name: rejected $description"
  fi
}

# 1. getOption stops depending on its argument.
prepare getoption_true
python3 - "$work/getoption_true/td/telegram/SynchronousRequests.cpp" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
text = text.replace(
    "      return OptionManager::is_synchronous_option(static_cast<const td_api::getOption *>(function)->name_);",
    "      return true;")
open(path, "w").write(text)
PY
expect_failure getoption_true "getOption returning true"

# 2. The predicate stops returning true for its synchronous methods.
prepare case_break
python3 - "$work/case_break/td/telegram/SynchronousRequests.cpp" <<'PY'
import sys
path = sys.argv[1]
start = open(path).read().index("bool SynchronousRequests::is_synchronous_request")
head, tail = open(path).read()[:start], open(path).read()[start:]
tail = tail.replace("      return true;", "      break;", 1)
open(path, "w").write(head + tail)
PY
expect_failure case_break "a case without return true"

# 3. The default branch returns true.
prepare default_true
python3 - "$work/default_true/td/telegram/SynchronousRequests.cpp" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
text = text.replace("    default:\n      return false;", "    default:\n      return true;")
open(path, "w").write(text)
PY
expect_failure default_true "a default branch returning true"

# 4. An extra argument-dependent condition appears before the switch.
prepare extra_condition
python3 - "$work/extra_condition/td/telegram/SynchronousRequests.cpp" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
text = text.replace(
    "  switch (function->get_id()) {",
    "  if (function->get_id() == td_api::getMe::ID) return false;\n  switch (function->get_id()) {")
open(path, "w").write(text)
PY
expect_failure extra_condition "an extra predicate before the switch"

# 5. A new case is added without a do_request handler.
prepare orphan_case
python3 - "$work/orphan_case/td/telegram/SynchronousRequests.cpp" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
text = text.replace(
    "    case td_api::testReturnError::ID:\n      return true;",
    "    case td_api::testReturnError::ID:\n    case td_api::getMe::ID:\n      return true;")
open(path, "w").write(text)
PY
expect_failure orphan_case "a case with no declared handler"

# 6. is_synchronous_option stops consulting the list.
prepare option_lookup
python3 - "$work/option_lookup/td/telegram/OptionManager.cpp" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
text = text.replace(
    "  return td::contains(get_synchronous_options(), name);",
    "  return true;")
open(path, "w").write(text)
PY
expect_failure option_lookup "is_synchronous_option returning true"

echo
if [ "$failures" -ne 0 ]; then
  echo "FAILED: $failures of $cases fixtures were accepted"
  exit 1
fi
echo "All $cases negative fixtures rejected."
