<#
.SYNOPSIS
Checks a staged TDLib package, or records the manifest that describes it.

.DESCRIPTION
The Windows native job restores the staged TDLib package and its codegen build
from a cache instead of rebuilding TDLib. Those files decide whether the
external configure succeeds, so they are checked before the run decides to reuse
them, and the same list is checked whether the package was restored or just
built.

The codegen build is a CMake tree: too many files, and too unstable, to hash.
Only the pieces the external configure reads are checked there: the files it
reads, the configuration it needs, and the source tree and compiler it was made
from.

-Mode write belongs to the run that produced the package. It validates the tree
and writes Stage/tdlib-stage-manifest.txt, the baseline later runs compare
against.

-Mode check belongs to the run that restored the package. The manifest must
already be there and must describe exactly the files that are present, so that
a truncated or partial restore is rejected instead of half used.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Bootstrap,
    [Parameter(Mandatory = $true)][string]$SchemaPath,
    [Parameter(Mandatory = $true)][string]$SchemaHash,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$Compiler,
    [Parameter(Mandatory = $true)][ValidateSet('check', 'write')][string]$Mode
)

$ErrorActionPreference = 'Stop'

$manifestName = 'tdlib-stage-manifest.txt'

function Get-StagedRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $Path.Substring($Root.Length + 1).Replace('\', '/')
}

# Reads one 'NAME:TYPE=' entry out of a CMake cache. The empty strings are
# allowed on purpose: a real cache contains blank lines between its groups.
function Get-CacheValue {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Cache,
        [Parameter(Mandatory = $true)][string]$Pattern
    )
    $found = @($Cache | Where-Object { $_ -match $Pattern })
    if ($found.Count -eq 0) { return '' }
    return ($found[0] -replace $Pattern, '')
}

# Two paths that name the same file, as CMake compares them.
function Test-SamePath {
    param(
        [Parameter(Mandatory = $true)][string]$Left,
        [Parameter(Mandatory = $true)][string]$Right
    )
    [IO.Path]::GetFullPath($Left) -eq [IO.Path]::GetFullPath($Right)
}

# The external configure reads the codegen library, the schema compiler and the
# generated config header out of the bootstrap, and the imported package out of
# the stage.
$requiredFiles = @(
    (Join-Path $Stage 'lib/cmake/Td/TdStaticTargets.cmake'),
    (Join-Path $Stage 'lib/cmake/Td/TdConfigVersion.cmake'),
    (Join-Path $Stage 'include/td/telegram/Client.h'),
    (Join-Path $Stage 'include/td/telegram/td_api.h'),
    (Join-Path $Stage 'include/td/telegram/td_api_json.h'),
    (Join-Path $Bootstrap 'CMakeCache.txt'),
    (Join-Path $Bootstrap 'td/tdtl/tdtl.lib'),
    (Join-Path $Bootstrap 'td/td/generate/tl-parser/tl-parser.exe'),
    (Join-Path $Bootstrap 'td/tdutils/td/utils/config.h')
)
foreach ($file in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "the TDLib package is missing $file"
    }
}

# Codegen tools from another configuration would generate a different API, and
# the external configure refuses a codegen build made from another source tree or
# another compiler; checking here means such a build is redone instead of
# failing the job after the reuse decision.
$bootstrapCache = Get-Content (Join-Path $Bootstrap 'CMakeCache.txt')
foreach ($needle in @('TDLUA_BACKEND:STRING=native', 'TDLUA_BUNDLED_TDLIB:BOOL=ON')) {
    if ($bootstrapCache -notcontains $needle) {
        throw "the codegen build is not a bundled native build: $needle"
    }
}
$bootstrapSource = Get-CacheValue -Cache $bootstrapCache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL='
if (-not $bootstrapSource) {
    throw 'the codegen build does not record the source tree it was made from'
}
if (-not (Test-SamePath -Left $bootstrapSource -Right $SourceDirectory)) {
    throw "the codegen build was made from $bootstrapSource, not from $SourceDirectory"
}
$bootstrapCompiler = Get-CacheValue -Cache $bootstrapCache `
    -Pattern '^CMAKE_CXX_COMPILER:(FILEPATH|STRING)='
if (-not $bootstrapCompiler) {
    throw 'the codegen build does not record the compiler it was made with'
}
if (-not (Test-SamePath -Left $bootstrapCompiler -Right $Compiler)) {
    throw "the codegen build was made with $bootstrapCompiler, not with $Compiler"
}
# Recorded in the run log: the comparison above is the one the external
# configure repeats, and reading it back is how a future mismatch is diagnosed.
"codegen build records source $bootstrapSource and compiler $bootstrapCompiler"

$versionPattern = 'set\(PACKAGE_VERSION "' + [regex]::Escape($Version) + '"\)'
if (-not (Select-String -Path (Join-Path $Stage 'lib/cmake/Td/TdConfigVersion.cmake') `
            -Pattern $versionPattern -Quiet)) {
    throw "the package does not report version $Version"
}

# The cache key pins the schema, but check it again: a package built from
# another td_api.tl must never be accepted as valid. The local name must differ
# from the parameter, because PowerShell variable names ignore case.
$actualSchemaHash = (Get-FileHash -Algorithm SHA256 $SchemaPath).Hash
if ($actualSchemaHash -ne $SchemaHash) {
    throw "the schema changed after provenance was recorded: $actualSchemaHash"
}

# Everything a consumer can read out of the package.
$stagedFiles = @()
$stagedFiles += Get-ChildItem (Join-Path $Stage 'lib') -File -Filter '*.lib'
$stagedFiles += Get-ChildItem (Join-Path $Stage 'lib/cmake/Td') -File -Filter '*.cmake'
$stagedFiles += Get-ChildItem (Join-Path $Stage 'include') -Recurse -File
if ($stagedFiles.Count -eq 0) {
    throw "the package has no file to describe: $Stage"
}
$stagedPaths = @($stagedFiles |
    ForEach-Object { Get-StagedRelativePath -Root $Stage -Path $_.FullName } |
    Sort-Object)

$manifestPath = Join-Path $Stage $manifestName

if ($Mode -eq 'check') {
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "the package carries no manifest: $manifestPath"
    }
    $entries = @(Get-Content $manifestPath)
    if ($entries.Count -eq 0) {
        throw "the manifest is empty: $manifestPath"
    }
    $listed = @{}
    foreach ($entry in $entries) {
        if ($entry -notmatch '^([0-9a-f]{64})  ([\w./-]+)$') {
            throw "the manifest has an unreadable line: $entry"
        }
        $expected = $Matches[1]
        $relative = $Matches[2]
        $path = Join-Path $Stage $relative
        if (-not (Test-Path -LiteralPath $path)) {
            throw "the package is missing $relative"
        }
        if ((Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant() -ne $expected) {
            throw "the package changed: $relative"
        }
        $listed[$relative.ToLowerInvariant()] = $true
    }
    # A manifest that kept its readable lines but lost others would otherwise
    # pass as a description of a package it no longer matches.
    foreach ($relative in $stagedPaths) {
        if (-not $listed.ContainsKey($relative.ToLowerInvariant())) {
            throw "the manifest does not list $relative"
        }
    }
    "the restored package matches its manifest ($($entries.Count) entries)"
}
else {
    $lines = @($stagedFiles |
        Sort-Object FullName |
        ForEach-Object {
            $relative = Get-StagedRelativePath -Root $Stage -Path $_.FullName
            "$((Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant())  $relative"
        })
    Set-Content -Path $manifestPath -Value $lines
    "wrote $($lines.Count) entries to $manifestPath"
}
