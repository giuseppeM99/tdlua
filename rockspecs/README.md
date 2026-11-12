# Rockspecs

LuaRocks specifications for TDLua, kept in one directory instead of the
repository root.

| File | Package | Purpose |
|---|---|---|
| `tdlua-scm-1.rockspec` | `tdlua` | development rock, tracks `master` |
| `tdlua-luajit-scm-1.rockspec` | `tdlua-luajit` | LuaJIT development rock, tracks `master` |
| `tdlua_eventloop-scm-1.rockspec` | `tdlua_eventloop` | event-loop development rock, tracks `master` |
| `tdlua-0.4.0-1.rockspec` … `tdlua-0.5.1-1.rockspec` | `tdlua` | historical versioned rocks |
| `tdlua-luajit-0.4.0-1.rockspec` … `tdlua-luajit-0.5.1-1.rockspec` | `tdlua-luajit` | historical versioned rocks |
| `tdlua_eventloop-0.6.0-1.rockspec`, `tdlua_eventloop-0.6.1-1.rockspec` | `tdlua_eventloop` | versioned event-loop rocks |

The `scm-1` rocks are the source-of-truth development specs. Release rocks
(`<package>-<version>-1.rockspec`) are generated from them by
`tools/release/generate-rockspec.lua` during the release workflow and are not
added to the checkout.

## Usage

Run LuaRocks from the repository root so the rockspec `build_command`'s
`cmake -S .` sees the source tree:

```sh
luarocks lint rockspecs/tdlua-scm-1.rockspec
luarocks make rockspecs/tdlua-scm-1.rockspec
```

The optional event-loop package is separate and never becomes a dependency of
the core rock:

```sh
luarocks make rockspecs/tdlua_eventloop-scm-1.rockspec
```

## Adding a release

Do not add a generated release rockspec here: the release workflow rewrites the
`scm-1` file into `release-meta/` and publishes it as a GitHub release asset.
