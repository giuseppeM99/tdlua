#!/usr/bin/env bash
# Static frame check for the R12-A separation (Linux/GCC/Clang side).
#
# This is the structural counterpart of the MSVC gate, which checks the
# $cppxdata$/$ip2state$/$tryMap$ symbols emitted for a function with EH state.
# GCC and Clang encode the same fact in a COMDAT section named after the
# function:
#
#   .gcc_except_table.<mangled>   the function has a landing pad / personality
#
# The checks are:
#   1. prepareManagedOnComplete exists out of line (noinline honoured);
#   2. it has an exception table, i.e. it owns and catches;
#   3. managedOnComplete calls it;
#   4. managedOnComplete has no exception table at all, so its frame owns
#      nothing and catches nothing, even after inlining the emission helper.
#
# Usage: check-on-complete-frames.sh <include-dir> <lua-include-dir> [source]
set -eu
# readelf and objdump localize their headers; parse the C locale only.
export LC_ALL=C
CXX=${CXX:-g++}
INC=${1:?include directory}
LUA_INC=${2:?lua include directory}
SRC=${3:-$(dirname "$0")/second_longjmp_hook_probe.cpp}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

"$CXX" -std=c++14 -O2 -DTDLUA_TESTING -I"$INC" -I"$LUA_INC" -c "$SRC" \
    -o "$TMP/probe.o"

PREPARE='_ZN5tdlua24prepareManagedOnCompleteEP9lua_StatePNS_13ManagedHandleE'
EMIT='_ZN5tdlua17managedOnCompleteEP9lua_StatePKc'

sections=$(readelf -SW "$TMP/probe.o" | awk '{print $2}')
# Only the relocations that belong to managedOnComplete's own .text section.
# The exception table of prepareManagedOnComplete has relocations of its own,
# so a global grep would pass even if the caller never called it.
emit_relocs=$(readelf -rW "$TMP/probe.o" | awk -v hit=".rela.text.$EMIT" '
    /^Relocation section/ { show = index($0, hit) > 0; next }
    show { print }
')

fail=0

if ! printf '%s\n' "$sections" | grep -qx ".text.$PREPARE"; then
    echo "FAIL: prepareManagedOnComplete is not emitted out of line" >&2
    fail=1
fi

if ! printf '%s\n' "$sections" | grep -qx ".gcc_except_table.$PREPARE"; then
    echo "FAIL: prepareManagedOnComplete has no exception table (lost ownership?)" >&2
    fail=1
fi

if ! printf '%s\n' "$emit_relocs" | grep -q "$PREPARE"; then
    echo "FAIL: managedOnComplete does not call prepareManagedOnComplete" >&2
    fail=1
fi

if printf '%s\n' "$sections" | grep -qx ".gcc_except_table.$EMIT"; then
    echo "FAIL: managedOnComplete has its own exception table" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "FRAME_CHECK_FAILED"
    exit 1
fi
echo "FRAME_CHECK_OK"
