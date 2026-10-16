param(
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [Parameter(Mandatory = $true)][string]$InstallDirectory,
    [switch]$AddressSanitizer
)

$ErrorActionPreference = 'Stop'
$luaVersion = '5.4.8'
$archiveName = "lua-$luaVersion.tar.gz"
$archiveUrl = "https://www.lua.org/ftp/$archiveName"
$archiveSha256 = '4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae'

New-Item -ItemType Directory -Force -Path $WorkDirectory, $InstallDirectory | Out-Null
$archive = Join-Path $WorkDirectory $archiveName
Invoke-WebRequest -Uri $archiveUrl -OutFile $archive
$actualHash = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
if ($actualHash -ne $archiveSha256) {
    throw "Lua source archive SHA-256 mismatch: $actualHash"
}

$sourceRoot = Join-Path $WorkDirectory "lua-$luaVersion"
if (Test-Path $sourceRoot) { Remove-Item -Recurse -Force $sourceRoot }
tar.exe -xzf $archive -C $WorkDirectory
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }

$includeDirectory = Join-Path $sourceRoot 'src'
$library = Join-Path $InstallDirectory 'lua54.lib'
$dll = Join-Path $InstallDirectory 'lua54.dll'
$interpreter = Join-Path $InstallDirectory 'lua.exe'
$coreSources = @(
    'lapi.c', 'lcode.c', 'lctype.c', 'ldebug.c', 'ldo.c', 'ldump.c',
    'lfunc.c', 'lgc.c', 'llex.c', 'lmem.c', 'lobject.c', 'lopcodes.c',
    'lparser.c', 'lstate.c', 'lstring.c', 'ltable.c', 'ltm.c', 'lundump.c',
    'lvm.c', 'lzio.c', 'lauxlib.c', 'lbaselib.c', 'lcorolib.c', 'ldblib.c',
    'liolib.c', 'lmathlib.c', 'loadlib.c', 'loslib.c', 'lstrlib.c',
    'ltablib.c', 'lutf8lib.c', 'linit.c'
) | ForEach-Object { Join-Path $includeDirectory $_ }

$compilerFlags = @('/nologo', '/MD', '/O2')
$dllLinkFlags = @("/OUT:$dll", "/IMPLIB:$library")
$exeLinkFlags = @("/OUT:$interpreter")
if ($AddressSanitizer) {
    $compilerFlags += @('/Zi', '/fsanitize=address')
    $dllLinkFlags += @('/DEBUG', '/INCREMENTAL:NO')
    $exeLinkFlags += @('/DEBUG', '/INCREMENTAL:NO')
}

& cl.exe @compilerFlags /LD /DLUA_BUILD_AS_DLL /DLUA_CORE /DLUA_LIB `
    "/I$includeDirectory" @coreSources "/link" @dllLinkFlags
if ($LASTEXITCODE -ne 0) { throw "Lua DLL build failed with exit code $LASTEXITCODE" }

& cl.exe @compilerFlags /DLUA_BUILD_AS_DLL "/I$includeDirectory" `
    (Join-Path $includeDirectory 'lua.c') "/link" $library @exeLinkFlags
if ($LASTEXITCODE -ne 0) { throw "Lua interpreter build failed with exit code $LASTEXITCODE" }

$versionOutput = & $interpreter -v 2>&1
if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch 'Lua 5\.4\.8') {
    throw "Unexpected Lua interpreter result: $versionOutput"
}

Copy-Item (Join-Path $includeDirectory 'lua.h') $InstallDirectory
Copy-Item (Join-Path $includeDirectory 'luaconf.h') $InstallDirectory
Copy-Item (Join-Path $includeDirectory 'lauxlib.h') $InstallDirectory
Copy-Item (Join-Path $includeDirectory 'lualib.h') $InstallDirectory
Copy-Item (Join-Path $includeDirectory 'lua.hpp') $InstallDirectory

Get-FileHash -Algorithm SHA256 $archive, $dll, $library, $interpreter |
    Format-Table -AutoSize | Out-String | Write-Output
Write-Output "LUA_INCLUDE_DIR=$InstallDirectory"
Write-Output "LUA_LIBRARY=$library"
Write-Output "LUA_EXECUTABLE=$interpreter"
