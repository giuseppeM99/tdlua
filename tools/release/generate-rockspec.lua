#!/usr/bin/env lua
-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Generate a versioned LuaRocks release rockspec from an SCM rockspec.
--
-- dist.info is the single source of truth for the release version. LuaRocks
-- only accepts a rockspec whose filename is <package>-<version>-<revision>.rockspec,
-- so the generated file is named from the package and version it declares
-- rather than from the git tag.
--
-- The release rockspec points at the published source tarball instead of the
-- git repository: the tarball is the only artifact that carries the pinned
-- td and base64 submodules that a bundled TDLib build needs.
--
-- The transformation is textual on purpose: rewriting the rockspec as a Lua
-- table would drop comments and reorder fields. Only the version line and the
-- source block change; everything else is copied verbatim.

local SCM_VERSION = '^version%s*=%s*"scm%-1"%s*$'
local PACKAGE = '^package%s*=%s*"([^"]+)"%s*$'
local DIST_VERSION = '^version%s*=%s*"([^"]+)"%s*$'
local SOURCE_OPEN = '^source%s*=%s*{%s*$'
local SOURCE_CLOSE = '^}%s*$'
local ROCKS_FILE = '^[%w_.+-]+%-%d+%.%d+%.%d+%-%d+%.rockspec$'

local function fail(message)
  io.stderr:write('generate-rockspec: ', message, '\n')
  os.exit(1)
end

local function read_file(path)
  local handle, err = io.open(path, 'r')
  if not handle then
    fail('cannot read ' .. path .. ': ' .. tostring(err))
  end
  local data = handle:read('a')
  handle:close()
  return data
end

local function write_file(path, data)
  local handle, err = io.open(path, 'w')
  if not handle then
    fail('cannot write ' .. path .. ': ' .. tostring(err))
  end
  handle:write(data)
  handle:close()
end

local function split_lines(text)
  text = text:gsub('\n$', '')
  local lines = {}
  for line in (text .. '\n'):gmatch('(.-)\n') do
    lines[#lines + 1] = line
  end
  return lines
end

-- Lua patterns anchor ^ and $ to the whole string, so line-scoped matches
-- go through this helper rather than :match() on the full text.
local function match_line(text, pattern)
  for _, line in ipairs(split_lines(text)) do
    local captured = line:match(pattern)
    if captured then
      return captured
    end
  end
  return nil
end

local function read_base_version(path)
  local version = match_line(read_file(path), DIST_VERSION)
  if not version then
    fail('cannot read the release version from ' .. path)
  end
  return version
end

local function source_url(tag)
  return string.format(
    'https://github.com/giuseppeM99/tdlua/releases/download/%s/tdlua-%s-source.tar.gz',
    tag, tag)
end

-- Returns the package name and the rewritten rockspec text.
local function generate(text, version, url)
  local package = match_line(text, PACKAGE)
  if not package then
    fail('the rockspec does not declare a package name')
  end

  local lines = split_lines(text)
  local output = {}
  local version_replaced = false
  local source_replaced = false

  local index = 1
  while index <= #lines do
    local line = lines[index]

    if not version_replaced and line:match(SCM_VERSION) then
      output[#output + 1] = string.format('version = "%s"', version)
      version_replaced = true
      index = index + 1

    elseif not source_replaced and line:match(SOURCE_OPEN) then
      local closed = false
      repeat
        index = index + 1
        if index > #lines then
          fail('the source block is not terminated')
        end
        closed = lines[index]:match(SOURCE_CLOSE) ~= nil
      until closed
      output[#output + 1] = 'source = {'
      output[#output + 1] = string.format('   url = "%s"', url)
      output[#output + 1] = '}'
      source_replaced = true
      index = index + 1

    else
      output[#output + 1] = line
      index = index + 1
    end
  end

  if not version_replaced then
    fail('the rockspec does not declare version = "scm-1"')
  end
  if not source_replaced then
    fail('the rockspec does not declare a source block')
  end

  return package, table.concat(output, '\n') .. '\n'
end

local function script_repository_root()
  local script = arg[0] or ''
  local directory = script:match('^(.*)/[^/]*$') or '.'
  return directory .. '/../..'
end

local function parse_arguments(argv)
  local known = {
    ['--rockspec'] = true,
    ['--output-dir'] = true,
    ['--dist-info'] = true,
    ['--base-version'] = true,
    ['--tag'] = true,
  }
  local options = {}
  local index = 1
  while index <= #argv do
    local name = argv[index]
    if not known[name] then
      fail('unknown argument ' .. tostring(name))
    end
    local value = argv[index + 1]
    if not value then
      fail(name .. ' requires a value')
    end
    options[name] = value
    index = index + 2
  end
  return options
end

local function main(argv)
  local options = parse_arguments(argv)

  local rockspec = options['--rockspec'] or fail('--rockspec is required')
  local output_dir = options['--output-dir'] or fail('--output-dir is required')
  local dist_info = options['--dist-info']
    or (script_repository_root() .. '/dist.info')

  local base_version = options['--base-version'] or read_base_version(dist_info)
  local tag = options['--tag'] or ('v' .. base_version)

  local package, text = generate(read_file(rockspec), base_version .. '-1',
    source_url(tag))

  local filename = string.format('%s-%s-1.rockspec', package, base_version)
  if not filename:match(ROCKS_FILE) then
    fail('generated filename is not a LuaRocks rockspec name: ' .. filename)
  end

  local output = output_dir .. '/' .. filename
  write_file(output, text)
  print(output)
end

main(arg)
