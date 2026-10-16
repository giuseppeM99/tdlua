# P5C.4b diagnostic harness. Forensics only: it does not build or change any
# product behavior, and it does not modify tests/managed_routing_oom_test.cpp
# (that file is compiled and run byte-identical as the control).
#
# What it produces, on the original MSVC Release configuration (/O2 /Ob2
# /DNDEBUG /MD /EHsc, Lua 5.4.8 built with debug symbols):
#   1. the control failure: the unmodified test run in isolated processes,
#      with its exit code and its first guard diagnostic;
#   2. the probe measurement: the sign of the leaked guard counter at the
#      failing offset (skipped versus double destruction), the guard
#      construction/destruction events, and the allocations that were rejected;
#   3. the remaining sweeps (task creation/execution/nested OOM) run separately,
#      so the continuation path is exercised without the corrupted heap of the
#      guard sweep;
#   4. the error path under CDB: the frame that raises the Lua error, the stack
#      that the non-local jump abandons, every SchedulerCore lifetime event and
#      the stacks of the guard destructor invocations;
#   5. the cleanup funclets owned by waitForResponse and by
#      managedWaitContinuation in the disassembly, plus an /EHs comparison
#      build as a diagnostic control for the /EHc assumption.
#
# This harness must be dropped before anything is merged.
param(
    [Parameter(Mandatory = $true)][string]$LuaDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$Iterations = 3,
    [int]$Offsets = 8,
    [int]$RepeatProbe = 3
)
$ErrorActionPreference = 'Stop'
$root = $env:GITHUB_WORKSPACE
$out = $OutputDirectory
$work = Join-Path $env:RUNNER_TEMP 'p5c4b-oom'
New-Item -ItemType Directory -Force $out, $work | Out-Null
$lua = $LuaDirectory
$env:PATH = "$lua;$work;$env:PATH"
$notes = New-Object System.Collections.Generic.List[string]

trap {
    @("message=$($_.Exception.Message)",
      "type=$($_.Exception.GetType().FullName)",
      "line=$($_.InvocationInfo.Line.Trim())",
      "position=$($_.InvocationInfo.PositionMessage)",
      "stack=$($_.ScriptStackTrace)") |
        Set-Content (Join-Path $out 'harness-error.txt')
    $notes | Set-Content (Join-Path $out 'notes.txt')
    throw
}

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

function Invoke-OomBinary {
    param([string]$Exe, [int]$MaxOffset, [switch]$All, [string]$Log)
    $arguments = @($MaxOffset)
    if ($All) { $arguments += 'all' }
    & $Exe @arguments *> $Log
    "exit=$LASTEXITCODE" | Add-Content $Log
    return $LASTEXITCODE
}

function Read-ProbeFacts {
    param([string]$Log)
    $facts = [pscustomobject]@{
        offsets = @(); rejections = @(); guardEvents = @(); phases = @()
        stopped = $null; exit = $null
    }
    foreach ($line in Get-Content $Log) {
        if ($line -match '^P5C4B offset=(\d+) guards=(-?\d+)') {
            $facts.offsets += $line
        }
        elseif ($line -match '^P5C4B reject ') {
            $facts.rejections += $line
        }
        elseif ($line -match '^P5C4B trace: guard ') {
            $facts.guardEvents += $line
        }
        elseif ($line -match '^P5C4B (phase|stopped)') {
            $facts.phases += $line
        }
        elseif ($line -match '^exit=(\d+)') {
            $facts.exit = $Matches[1]
        }
    }
    return $facts
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
    $log = Join-Path $work "control-direct-$iteration.log"
    $codes += Invoke-OomBinary -Exe $control -MaxOffset $Offsets -Log $log
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

# --- 2. probe measurement: guard sign and rejected allocations -------------
$probeLog = Join-Path $out 'probe-run.log'
$probeCode = Invoke-OomBinary -Exe $probe -MaxOffset $Offsets -Log $probeLog
$probeFacts = Read-ProbeFacts -Log $probeLog
@("probe=tools/diagnostics/p5c4b_oom_probe.cpp",
  "flags=/O2 /Ob2 /DNDEBUG /MD /EHsc /DTDLUA_TESTING",
  "probe_exit=$probeCode") | Set-Content (Join-Path $out 'probe-summary.txt')
$probeFacts.offsets | Add-Content (Join-Path $out 'probe-summary.txt')
$probeFacts.rejections | Add-Content (Join-Path $out 'probe-rejections.txt')
$probeFacts.rejections | Add-Content (Join-Path $out 'probe-summary.txt')

# Sign of the leaked counter: a positive value means the destructor was
# skipped while the object was alive, a negative one means the cleanup funclet
# ran a second time on an object that was already destroyed.
$guardSign = @()
foreach ($line in $probeFacts.offsets) {
    if ($line -match '^P5C4B offset=(\d+) guards=(-?\d+)') {
        $guardSign += [int]::Parse($Matches[2])
    }
}
$positive = @($guardSign | Where-Object { $_ -gt 0 }).Count
$negative = @($guardSign | Where-Object { $_ -lt 0 }).Count
@("offsets_with_canary=$($guardSign.Count)",
  "guards_positive_skipped=$positive",
  "guards_negative_double_destruction=$negative") |
    Add-Content (Join-Path $out 'probe-summary.txt')
$notes.Add("probe: canary offsets=$($guardSign.Count); positive/skipped=$positive; " +
           "negative/double=$negative")

# --- 3. trace probe, repeated: guard events and wait phases ----------------
$traceCodes = @()
for ($iteration = 1; $iteration -le $RepeatProbe; $iteration++) {
    $log = Join-Path $work "trace-guard-$iteration.log"
    $traceCodes += Invoke-OomBinary -Exe $probeTrace -MaxOffset 1 -Log $log
}
Copy-Item (Join-Path $work 'trace-guard-1.log') (Join-Path $out 'trace-guard-first.log')
@("repeats=$($traceCodes.Count)",
  "exit_codes=$($traceCodes -join ',')") | Set-Content (Join-Path $out 'trace-guard-summary.txt')
$traceFacts = Read-ProbeFacts -Log (Join-Path $work 'trace-guard-1.log')
$traceFacts.guardEvents | Add-Content (Join-Path $out 'trace-guard-summary.txt')
$traceFacts.phases | Add-Content (Join-Path $out 'trace-guard-summary.txt')
$dtorEvents = @($traceFacts.guardEvents | Where-Object { $_ -match 'guard dtor' })
$ctorEvents = @($traceFacts.guardEvents | Where-Object { $_ -match 'guard ctor' })
@("guard_ctors=$($ctorEvents.Count)",
  "guard_dtors=$($dtorEvents.Count)") | Add-Content (Join-Path $out 'trace-guard-summary.txt')
$notes.Add("trace probe: guard ctors=$($ctorEvents.Count) dtors=$($dtorEvents.Count) on the failing offset")

# --- 4. the remaining sweeps: the continuation path in isolation -----------
$restLog = Join-Path $out 'probe-rest.log'
$restCode = Invoke-OomBinary -Exe $probeTrace -MaxOffset 0 -All -Log $restLog
@("probe=tools/diagnostics/p5c4b_oom_probe.cpp",
  "mode=0 all (sweep skipped)",
  "exit=$restCode") | Set-Content (Join-Path $out 'probe-rest-summary.txt')
Select-String -Path $restLog -Pattern '^P5C4B step|^P5C4B probe done|^Task |^Nested |^Targeted |^exit=' |
    ForEach-Object { $_.Line } | Add-Content (Join-Path $out 'probe-rest-summary.txt')

# --- 5. /EHs diagnostic comparison ----------------------------------------
$standardLog = Join-Path $out 'probe-ehs-guard.log'
$standardCode = Invoke-OomBinary -Exe $probeStandard -MaxOffset 1 -Log $standardLog
@("flags=/O2 /Ob2 /DNDEBUG /MD /EHs /DTDLUA_TESTING (diagnostic only)",
  "exit=$standardCode") | Set-Content (Join-Path $out 'probe-ehs-summary.txt')
Select-String -Path $standardLog -Pattern '^P5C4B offset|^P5C4B reject|^exit=' |
    ForEach-Object { $_.Line } | Add-Content (Join-Path $out 'probe-ehs-summary.txt')
$standardRestLog = Join-Path $out 'probe-ehs-rest.log'
$standardRestCode = Invoke-OomBinary -Exe $probeStandard -MaxOffset 0 -All -Log $standardRestLog
"rest_exit=$standardRestCode" | Add-Content (Join-Path $out 'probe-ehs-summary.txt')

# --- 6. cleanup funclets in the disassembly --------------------------------
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
        $destination = Join-Path $out "funclets-$name-$function.txt"
        $funclets = @(Select-String -Path $file -CaseSensitive -Pattern '::`1''::')
        @("binary=$name function=$function funclets=$($funclets.Count)") | Set-Content $destination
        $funclets | ForEach-Object { $_.Line.Trim() } | Add-Content $destination
        @(Select-String -Path $file -Pattern 'lua_error|lua_yieldk|lua_settop') |
            ForEach-Object { $_.Line.Trim() } | Add-Content $destination
    }
}

# --- 7. CDB: the error path, the abandoned stack, the core lifetime --------
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

# Symbol discovery: records whether the fixture's Guard methods survived /O2,
# where the frame that raises the Lua error is, and whether Lua's static
# throw helper is available.
$symbolLog = Join-Path $out 'cdb-symbols.log'
& "$debuggers\cdb.exe" -o -y $symbolPath `
    -logo (Join-Path $out 'cdb-symbols-debugger.log') `
    -c "x $traceName!*Guard*; x $traceName!*waitForResponse*; x lua54!luaD_throw; x lua54!lua_error; q" `
    $traceExe 1 *> $symbolLog

$extraBreakpoints = New-Object System.Collections.Generic.List[string]
foreach ($line in Get-Content $symbolLog) {
    if ($line -match '!(\?\?1Guard@[^ ]*)') {
        $extraBreakpoints.Add('bu ' + $traceName + '!' + $Matches[1] +
            ' ".echo P5C4B-GUARD-DTOR; kb 20; g"')
    }
}

function Invoke-Trace {
    param([string]$Label, [int]$MaxOffset, [switch]$All)
    $script = Join-Path $out "trace-commands-$Label.txt"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('sxd av')
    $lines.Add('sxe 0xc0000374')
    $lines.Add('.reload')
    $lines.Add('bu lua54!luaD_throw ".echo P5C4B-RAISE-luaD_throw; kb 40; g"')
    $lines.Add('bu lua54!lua_error ".echo P5C4B-RAISE-lua_error; kb 30; g"')
    $lines.Add('bu lua54!luaL_error ".echo P5C4B-RAISE-luaL_error; kb 30; g"')
    $lines.Add('bu ' + $traceName + '!tdlua::SchedulerCore::SchedulerCore ".echo P5C4B-CORE-CTOR; r @rcx; kb 12; g"')
    $lines.Add('bu ' + $traceName + '!tdlua::SchedulerCore::~SchedulerCore ".echo P5C4B-CORE-DTOR; r @rcx; kb 12; g"')
    foreach ($breakpoint in $extraBreakpoints) { $lines.Add($breakpoint) }
    $lines.Add('bl')
    $lines.Add('g')
    $lines.Add('.echo ===END-OF-TRACE===')
    $lines.Add('q')
    $lines | Set-Content $script
    $arguments = @($MaxOffset)
    if ($All) { $arguments += 'all' }
    Push-Location $work
    try {
        & "$debuggers\cdb.exe" -o -y $symbolPath `
            -logo (Join-Path $out "cdb-$Label-debugger.log") `
            -cf $script "$traceName.exe" @arguments *> (Join-Path $out "cdb-$Label.log")
        "cdb_exit=$LASTEXITCODE" | Add-Content (Join-Path $out "cdb-$Label.log")
    }
    finally { Pop-Location }
    if (-not (Select-String -Path (Join-Path $out "cdb-$Label.log") -Pattern '===END-OF-TRACE===')) {
        $notes.Add("WARNING: the $Label trace did not reach the end of the trace")
    }
}

Invoke-Trace -Label 'guard' -MaxOffset 1
Invoke-Trace -Label 'rest' -MaxOffset 0 -All

# --- verdict --------------------------------------------------------------
# The raw disassemblies are derived evidence and unnecessarily large: the
# extracted per-function bodies above carry the funclet names.
Remove-Item (Join-Path $out 'disasm-*.txt') -Force
$notes | Set-Content (Join-Path $out 'notes.txt')
'P5C.4b managed routing OOM forensics complete' |
    Set-Content (Join-Path $out 'result.txt')
