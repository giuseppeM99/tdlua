# P5C.4b diagnostic harness. Forensics only: it does not build or change any
# product behavior, and it does not modify tests/managed_routing_oom_test.cpp
# (that file is compiled and run byte-identical as the control).
#
# What it produces, on the original MSVC Release configuration (/O2 /Ob2
# /DNDEBUG /MD /EHsc, Lua 5.4.8 with debug symbols):
#   1. the control failure: the unmodified test run in an isolated process,
#      captured with its exit code and its first-guard diagnostic;
#   2. the probe measurement: the number of offsets whose guard canary fires,
#      the sign of the leaked counter (skipped versus double destruction) and
#      the rejected allocation that starts the fault;
#   3. the error path under CDB: the frame that raises the Lua error, the stack
#      that the non-local jump abandons and every SchedulerCore lifetime event;
#   4. the cleanup funclets owned by waitForResponse and by
#      managedWaitContinuation in the disassembly, plus the /EHs comparison
#      build as a diagnostic control for the /EHc assumption.
#
# This harness must be dropped before anything is merged.
param(
    [Parameter(Mandatory = $true)][string]$LuaDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$Iterations = 3,
    [int]$Offsets = 8
)
$ErrorActionPreference = 'Stop'
$root = $env:GITHUB_WORKSPACE
$out = $OutputDirectory
$work = Join-Path $env:RUNNER_TEMP 'p5c4b-oom'
New-Item -ItemType Directory -Force $out, $work | Out-Null
$lua = $LuaDirectory
$env:PATH = "$lua;$work;$env:PATH"
$notes = New-Object System.Collections.Generic.List[string]

& cl.exe /Bv 2>&1 | Out-File (Join-Path $out 'compiler.txt')
(& git rev-parse HEAD | Out-String).Trim() | Out-File (Join-Path $out 'source.txt')
(& git diff --stat | Out-String).Trim() | Out-File (Join-Path $out 'worktree-diff.txt')
(& git rev-parse --abbrev-ref HEAD | Out-String).Trim() | Out-File (Join-Path $out 'branch.txt')
Get-FileHash (Join-Path $lua 'lua54.dll'), (Join-Path $lua 'lua54.lib') |
    Format-List | Out-File (Join-Path $out 'lua-hashes.txt')

function Build-OomBinary {
    param([string]$Name, [string]$Source, [string[]]$ExtraFlags = @())
    $exe = Join-Path $work "$Name.exe"
    $pdb = Join-Path $work "$Name.pdb"
    $arguments = @('/nologo', '/MD', '/Zi', '/O2', '/Ob2', '/DNDEBUG',
                   '/DTDLUA_TESTING') + $ExtraFlags +
                 @("/I$root\include", "/I$lua", $Source, "/Fe:$exe", "/Fd:$pdb",
                   '/link', "$lua\lua54.lib", '/DEBUG', '/INCREMENTAL:NO',
                   "/PDB:$pdb")
    & cl.exe @arguments *> (Join-Path $out "build-$Name.log")
    if ($LASTEXITCODE -ne 0) { throw "build $Name failed: $LASTEXITCODE" }
    ('flags=' + ($ExtraFlags -join ' ')) | Add-Content (Join-Path $out "build-$Name.log")
    return $exe
}

# --- builds ---------------------------------------------------------------
$control = Build-OomBinary -Name 'control-ehsc' `
    -Source "$root\tests\managed_routing_oom_test.cpp" -ExtraFlags @('/EHsc')
$probe = Build-OomBinary -Name 'probe-ehsc' `
    -Source "$root\tools\diagnostics\p5c4b_oom_probe.cpp" -ExtraFlags @('/EHsc')
$probeTrace = Build-OomBinary -Name 'probe-ehsc-trace' `
    -Source "$root\tools\diagnostics\p5c4b_oom_probe.cpp" `
    -ExtraFlags @('/EHsc', '/DP5C4B_TRACE')
$probeStandard = Build-OomBinary -Name 'probe-ehs-trace' `
    -Source "$root\tools\diagnostics\p5c4b_oom_probe.cpp" `
    -ExtraFlags @('/EHs', '/DP5C4B_TRACE')

# --- 1. control: the unmodified test, isolated processes -------------------
$codes = @()
for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
    & $control *> (Join-Path $work "control-direct-$iteration.log")
    $codes += $LASTEXITCODE
}
$failed = @($codes | Where-Object { $_ -ne 0 }).Count
@("source=tests/managed_routing_oom_test.cpp (unmodified)",
  "flags=/O2 /Ob2 /DNDEBUG /MD /EHsc /DTDLUA_TESTING",
  "iterations_run=$($codes.Count)",
  "non_zero_exit=$failed",
  "exit_codes=$($codes -join ',')") |
    Set-Content (Join-Path $out 'control-summary.txt')
Copy-Item (Join-Path $work 'control-direct-1.log') (Join-Path $out 'control-first.log')
$notes.Add("control: $failed of $($codes.Count) isolated runs exited non-zero")
if ($failed -eq 0) { $notes.Add('WARNING: control did not fail; reproducer not confirmed') }

# --- 2. probe measurement: guard sign and rejected allocation --------------
& $probe $Offsets *> (Join-Path $out 'probe-run.log')
$probeCode = $LASTEXITCODE
"probe_exit=$probeCode" | Add-Content (Join-Path $out 'probe-run.log')
$offsets = @()
$rejections = @()
foreach ($line in Get-Content (Join-Path $out 'probe-run.log')) {
    if ($line -match '^P5C4B offset=(\d+) guards=(-?\d+)') {
        $offsets += [pscustomobject]@{ offset = [int]$Matches[1]; guards = [int]$Matches[2]; line = $line }
    }
    if ($line -match '^P5C4B reject call=(\d+) size=(\d+) old_size=(\d+)') {
        $rejections += $line
    }
}
$guardFired = @($offsets | Where-Object { $_.guards -ne 0 })
$skipped = @($offsets | Where-Object { $_.guards -gt 0 })
$doubled = @($offsets | Where-Object { $_.guards -lt 0 })
@("probe=tools/diagnostics/p5c4b_oom_probe.cpp",
  "offsets_swept=$($offsets.Count)",
  "guard_canary_fired=$($guardFired.Count)",
  "guards_positive_skipped=$($skipped.Count)",
  "guards_negative_double_destruction=$($doubled.Count)",
  "probe_exit=$probeCode") |
    Set-Content (Join-Path $out 'probe-summary.txt')
$offsets | ForEach-Object { $_.line } | Add-Content (Join-Path $out 'probe-summary.txt')
$rejections | Set-Content (Join-Path $out 'probe-rejections.txt')
$notes.Add("probe: guard canary fired on $($guardFired.Count) of $($offsets.Count) offsets " +
           "(positive/skipped=$($skipped.Count), negative/double=$($doubled.Count))")

# --- 3. /EHs diagnostic comparison ----------------------------------------
& $probeStandard 1 *> (Join-Path $out 'probe-ehs-run.log')
"probe_ehs_exit=$LASTEXITCODE" | Add-Content (Join-Path $out 'probe-ehs-run.log')

# --- 4. cleanup funclets in the disassembly --------------------------------
function Export-Disassembly {
    param([string]$Name, [string]$Exe)
    $asm = Join-Path $out "disasm-$Name.txt"
    dumpbin.exe /nologo /disasm:nobytes $Exe | Out-File $asm
    return $asm
}

function Export-Function {
    param([string]$Asm, [string]$Pattern, [string]$Destination)
    $collected = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in Get-Content $Asm) {
        if ($line -match '^\s*[0-9A-Fa-f]+:') {
            if ($inside) { $collected.Add($line) }
            continue
        }
        if ($line -match $Pattern) { $inside = $true; $collected.Add($line) }
        elseif ($inside) { $inside = $false }
    }
    $collected | Set-Content $Destination
    return $collected.Count
}

$summary = New-Object System.Collections.Generic.List[string]
foreach ($item in @(@('control-ehsc', $control), @('probe-ehsc-trace', $probeTrace))) {
    $name = $item[0]
    $asm = Export-Disassembly -Name $name -Exe $item[1]
    foreach ($function in @('waitForResponse', 'managedWaitContinuation',
                            'managedWait', 'prepareManagedWait')) {
        $destination = Join-Path $out "func-$name-$function.txt"
        $count = Export-Function -Asm $asm -Pattern "\?$function@" -Destination $destination
        $summary.Add("binary=$name function=$function lines=$count")
    }
}
$summary | Set-Content (Join-Path $out 'funclet-summary.txt')
foreach ($item in @(@('control-ehsc', $control), @('probe-ehsc-trace', $probeTrace))) {
    $name = $item[0]
    foreach ($function in @('waitForResponse', 'managedWaitContinuation')) {
        $file = Join-Path $out "func-$name-$function.txt"
        $funclets = @(Select-String -Path $file -CaseSensitive -Pattern '::`1''::')
        @("binary=$name function=$function funclets=$($funclets.Count)") |
            Set-Content (Join-Path $out "funclets-$name-$function.txt")
        $funclets | ForEach-Object { $_.Line.Trim() } |
            Add-Content (Join-Path $out "funclets-$name-$function.txt")
        $errors = @(Select-String -Path $file -Pattern 'lua_error|lua_yieldk|lua_settop')
        $errors | ForEach-Object { $_.Line.Trim() } |
            Add-Content (Join-Path $out "funclets-$name-$function.txt")
    }
}

# --- 5. CDB: the error path, the abandoned stack, the core lifetime --------
$debuggers = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64'
if (-not (Test-Path "$debuggers\cdb.exe")) {
    $installer = "$env:RUNNER_TEMP\winsdk-debuggers.exe"
    Invoke-WebRequest 'https://go.microsoft.com/fwlink/?linkid=2120843' -OutFile $installer
    $install = Start-Process $installer -ArgumentList '/features OptionId.WindowsDesktopDebuggers /quiet /norestart' -Wait -PassThru
    "debuggers installer exit=$($install.ExitCode)" | Out-File (Join-Path $out 'debuggers-install.txt')
    if ($install.ExitCode -notin 0, 3010) { throw 'Debugger-only installation failed' }
}
if (-not (Test-Path "$debuggers\cdb.exe")) { throw 'CDB unavailable' }
$symbolPath = "srv*$env:RUNNER_TEMP\p5c4b-sym*https://msdl.microsoft.com/download/symbols"
$traceName = 'probe-ehsc-trace'
$traceExe = $work + '\' + "$traceName.exe"

# Symbol discovery: records whether the fixture's Guard methods survived /O2
# and where the frame that raises the Lua error is.
& "$debuggers\cdb.exe" -o -y $symbolPath `
    -logo (Join-Path $out 'cdb-symbols-debugger.log') `
    -c "x $traceName!*Guard*; x $traceName!*waitForResponse*; x lua54!luaD_throw; x lua54!lua_error; q" `
    $traceExe 1 *> (Join-Path $out 'cdb-symbols.log')

$coreCtor = 'bu ' + $traceName + '!tdlua::SchedulerCore::SchedulerCore ".echo P5C4B-CORE-CTOR; r @rcx; kb 12; g"'
$coreDtor = 'bu ' + $traceName + '!tdlua::SchedulerCore::~SchedulerCore ".echo P5C4B-CORE-DTOR; r @rcx; kb 12; g"'
$traceScript = Join-Path $out 'trace-commands.txt'
@('sxd av',
  'sxe 0xc0000374',
  '.reload',
  'bu lua54!luaD_throw ".echo P5C4B-RAISE-luaD_throw; kb 40; g"',
  'bu lua54!lua_error ".echo P5C4B-RAISE-lua_error; kb 30; g"',
  'bu lua54!luaL_error ".echo P5C4B-RAISE-luaL_error; kb 30; g"',
  $coreCtor,
  $coreDtor,
  'bl',
  'g',
  '.echo ===END-OF-TRACE===',
  'q') | Set-Content $traceScript

Push-Location $work
try {
    & "$debuggers\cdb.exe" -o -y $symbolPath `
        -logo (Join-Path $out 'cdb-trace-debugger.log') `
        -cf $traceScript "$traceName.exe" 1 *> (Join-Path $out 'cdb-trace.log')
    "cdb_exit=$LASTEXITCODE" | Add-Content (Join-Path $out 'cdb-trace.log')
}
finally { Pop-Location }
if (-not (Select-String -Path (Join-Path $out 'cdb-trace.log') -Pattern '===END-OF-TRACE===')) {
    $notes.Add('WARNING: the traced run did not reach the end of the trace')
}

# --- verdict --------------------------------------------------------------
$notes | Set-Content (Join-Path $out 'notes.txt')
'P5C.4b managed routing OOM forensics complete' |
    Set-Content (Join-Path $out 'result.txt')
