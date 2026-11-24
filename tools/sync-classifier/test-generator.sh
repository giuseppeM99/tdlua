#!/bin/bash
# Negative fixtures for tools/sync-classifier/generate_sync_classifier.cmake.
#
# Each case copies the pinned TDLib sources, applies one change that alters the
# classification without changing the case labels, and requires the generator to
# fail. A generator that accepted it would emit the wrong method set.
#
# The script first proves that the unmodified sources are accepted, so a broken
# generator or a missing input cannot make every rejection look successful.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$script_dir/../.." && pwd)
gen="$script_dir/generate_sync_classifier.cmake"
td="$repo/td/td/telegram"

work=$(mktemp -d "${TMPDIR:-/tmp}/tdlua-classifier-fixtures.XXXXXX")
trap 'rm -rf "$work"' EXIT

for input in SynchronousRequests.cpp SynchronousRequests.h OptionManager.cpp; do
    if [ ! -f "$td/$input" ]; then
        echo "missing pinned TDLib source: $td/$input" >&2
        exit 1
    fi
done
if [ ! -f "$gen" ]; then
    echo "missing generator: $gen" >&2
    exit 1
fi

prepare() {
    mkdir -p "$work/$1/td/telegram"
    cp "$td/SynchronousRequests.cpp" "$td/SynchronousRequests.h" \
       "$td/OptionManager.cpp" "$work/$1/td/telegram/"
}

run_generator() {
    cmake -DTDLIB_SOURCE_DIR="$1" -DTDLIB_VERSION=1.8.71 \
          -DOUTPUT_DIR="$work/output-$2" -P "$gen"
}

# 0. The unmodified sources must be accepted.
prepare baseline
if ! run_generator "$work/baseline" baseline >"$work/baseline.log" 2>&1; then
    echo "the generator rejected the unmodified pinned sources" >&2
    cat "$work/baseline.log" >&2
    exit 1
fi
echo "ok    baseline: accepted"

# Applies a Python edit and fails if the marker it expects is absent, so a
# fixture can never silently become a no-op.
edit() {
    local name=$1
    shift
    python3 - "$name" "$@" <<'PY'
import sys

name, path, old, new = sys.argv[1:5]
text = open(path).read()
count = text.count(old)
# A marker that matches twice is ambiguous and may hit the wrong statement, so
# the fixture would test something other than what it claims.
if count != 1:
    raise SystemExit(
        f"fixture {name}: marker occurs {count} times, expected once: {old!r}")
open(path, "w").write(text.replace(old, new, 1))
PY
}

expect_failure() {
    local name=$1 description=$2
    if run_generator "$work/$name" "$name" >"$work/$name.log" 2>&1; then
        echo "FAIL  $name: generator accepted $description"
        failures=$((failures + 1))
    else
        echo "ok    $name: rejected $description"
    fi
}

failures=0
cases=0

# 1. getOption stops depending on its argument.
prepare getoption_true
edit getoption_true "$work/getoption_true/td/telegram/SynchronousRequests.cpp" \
    "      return OptionManager::is_synchronous_option(static_cast<const td_api::getOption *>(function)->name_);" \
    "      return true;"
cases=$((cases + 1)); expect_failure getoption_true "getOption returning true"

# 2. The synchronous methods stop returning true.
prepare case_break
edit case_break "$work/case_break/td/telegram/SynchronousRequests.cpp" \
    "    case td_api::testReturnError::ID:
      return true;" \
    "    case td_api::testReturnError::ID:
      break;"
cases=$((cases + 1)); expect_failure case_break "a case without return true"

# 3. A short-circuit makes the true-return ineffective.
prepare true_and_false
edit true_and_false "$work/true_and_false/td/telegram/SynchronousRequests.cpp" \
    "    case td_api::testReturnError::ID:
      return true;" \
    "    case td_api::testReturnError::ID:
      return true && false;"
cases=$((cases + 1)); expect_failure true_and_false "return true && false"

# 4. The option lookup is neutralised by a short circuit.
prepare option_short_circuit
edit option_short_circuit "$work/option_short_circuit/td/telegram/SynchronousRequests.cpp" \
    "      return OptionManager::is_synchronous_option(static_cast<const td_api::getOption *>(function)->name_);" \
    "      return OptionManager::is_synchronous_option(static_cast<const td_api::getOption *>(function)->name_) && false;"
cases=$((cases + 1)); expect_failure option_short_circuit "a getOption short circuit"

# 5. The default branch returns true.
prepare default_true
edit default_true "$work/default_true/td/telegram/SynchronousRequests.cpp" \
    "    default:
      return false;" \
    "    default:
      return true;"
cases=$((cases + 1)); expect_failure default_true "a default branch returning true"

# 6. An argument-dependent condition runs before the switch.
prepare extra_condition
edit extra_condition "$work/extra_condition/td/telegram/SynchronousRequests.cpp" \
    "  switch (function->get_id()) {" \
    "  if (function->get_id() == td_api::getMe::ID) return false;
  switch (function->get_id()) {"
cases=$((cases + 1)); expect_failure extra_condition "an extra predicate before the switch"

# 7. A case is added without a declared handler.
prepare orphan_case
edit orphan_case "$work/orphan_case/td/telegram/SynchronousRequests.cpp" \
    "    case td_api::testReturnError::ID:
      return true;" \
    "    case td_api::testReturnError::ID:
    case td_api::getMe::ID:
      return true;"
cases=$((cases + 1)); expect_failure orphan_case "a case with no declared handler"

# 8. A second default branch makes the trailing one unreachable.
prepare second_default
edit second_default "$work/second_default/td/telegram/SynchronousRequests.cpp" \
    "    case td_api::testReturnError::ID:
      return true;" \
    "    default:
      return false;
    case td_api::testReturnError::ID:
      return true;"
cases=$((cases + 1)); expect_failure second_default "a second default branch"

# 9. is_synchronous_option stops consulting the list.
prepare option_lookup
edit option_lookup "$work/option_lookup/td/telegram/OptionManager.cpp" \
    "  return td::contains(get_synchronous_options(), name);" \
    "  return true;"
cases=$((cases + 1)); expect_failure option_lookup "is_synchronous_option returning true"

# 10. The lookup gains an extra condition before the membership test.
prepare option_extra
edit option_extra "$work/option_extra/td/telegram/OptionManager.cpp" \
    "  return td::contains(get_synchronous_options(), name);" \
    "  if (name == \"my_id\") return true;
  return td::contains(get_synchronous_options(), name);"
cases=$((cases + 1)); expect_failure option_extra "an extra condition in the option lookup"

echo
if [ "$failures" -ne 0 ]; then
    echo "FAILED: $failures of $cases fixtures were accepted"
    exit 1
fi
echo "All $cases negative fixtures rejected."
