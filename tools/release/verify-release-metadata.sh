#!/bin/sh
# Copyright (c) 2018-2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause
#
# Verify the release metadata that must ship with the payload. Used by
# release-package.yml, release-dry.yml and release.yml so all three gates
# apply the same rules.
#
# Usage: verify-release-metadata.sh <release-notes.md> <lua-license.txt>

set -eu

notes=${1:?release notes path required}
license=${2:?Lua license path required}

fail() {
    echo "release metadata check failed: $*" >&2
    exit 1
}

[ -s "$notes" ] || fail "release notes are missing or empty: $notes"
[ -s "$license" ] || fail "Lua license is missing or empty: $license"

grep -q '^# TDLua ' "$notes" || fail "release notes have no TDLua title"

# The public body must contain the sections and limits that the release
# contract promises. The first list is inherited from the 0.6.0 contract and
# must keep holding; the second records the optional Copas surface added in
# 0.6.1. Keep both in sync with release-notes/v0.6.1.md.
for section in \
    '## Highlights' \
    '## What is new' \
    '## Runtime prerequisites' \
    '## Known limitations' \
    '## Licenses' \
    '### Unreliable transport and DEF-01' \
    'readiness' \
    'tdlua_eventloop.luv' \
    'tdlua_eventloop.cqueues' \
    'Lua 5.5.0' \
    'liblua5.2.so.0' \
    'liblua5.3.so.0' \
    'liblua5.4.so.0' \
    'libluajit-5.1.so.2' \
    'Lua-5.5-LICENSE.txt' \
    '1024' \
    '120 s' \
    'drop-oldest' \
    'Windows SOCKET reconnection' \
    'macOS and AArch64' \
    'cqueues runtime is verified on Lua 5.4 only' \
    'Authenticated Telegram' \
    'LuaSocket' \
    'No application migration'; do
    grep -qF "$section" "$notes" || fail "release notes missing: $section"
done

# 0.6.1 adds the optional Copas adapter and bridge. The public body must name
# the new modules and keep the ownership limits that make them safe to use.
for section in \
    'tdlua_eventloop.copas' \
    'tdlua_eventloop.copas_bridge' \
    'Driver:clear()' \
    'Future:wait()' \
    'waiter:cancel()' \
    'Copas 4.12.0' \
    'TDLib 1.8.70'; do
    grep -qF "$section" "$notes" || fail "release notes missing: $section"
done

# Lua 5.5.0 is linked statically into the Linux x86_64 Lua 5.5 modules. The
# notice must be the full MIT notice, not just a version string.
for phrase in \
    'Lua 5.5.0' \
    'Copyright' \
    'Permission is hereby granted' \
    'The above copyright notice and this permission notice shall be included' \
    'THE SOFTWARE IS PROVIDED "AS IS"'; do
    grep -qF "$phrase" "$license" || fail "Lua license missing: $phrase"
done

# The phrase checks above are readable diagnostics. The checksum is the
# authoritative completeness check: it rejects a notice that has been truncated
# or edited while still containing the expected substrings. Update it only when
# the pinned Lua version or the provenance header changes.
EXPECTED_LUA_LICENSE_SHA256=86234a3109d2ffd3d0cc537f0eaa221e9b15c54a40564373b608a1d8e777560c
actual_lua_license_sha256="$(sha256sum "$license" | awk '{print $1}')"
[ "$actual_lua_license_sha256" = "$EXPECTED_LUA_LICENSE_SHA256" ] ||
    fail "Lua license checksum mismatch: $actual_lua_license_sha256"

echo "release metadata verified: $notes, $license"
