# P5C.4b diagnostic harness. Forensics only: it does not build or change any
# product behavior, and it does not modify tests/managed_routing_oom_test.cpp
# (that file is compiled and run byte-identical as the control).
#
# What it produces, on the original MSVC Release configuration (/O2 /Ob2
# /DNDEBUG /MD /EHsc, Lua 5.4.8 built with debug symbols):
#   1. the control failure: the unmodified test run in isolated processes, with
#      its exit code and its first guard diagnostic;
#   2. the probe measurement: the sign of the leaked guard counter at the
#      failing offset (skipped versus double destruction), the guard
#      construction/destruction events, and the allocations that were rejected;
#   3. the same probe built with P5C4B_TRACE and with /EHs instead of /EHsc, as
#      diagnostic controls for the tracing and for the /EHc assumption;
#   4. the remaining sweeps (task creation/execution/nested OOM) run separately,
#      so the continuation path is exercised without the corrupted heap of the
#      guard sweep;
#   5. the cleanup funclets owned by waitForResponse and by
#      managedWaitContinuation in the disassembly;
#   6. the error path under CDB on the binary that reproduces: the frame that
#      raises the Lua error, the stack that the non-local jump abandons, every
#      SchedulerCore lifetime event and the stack of every invocation of the
#      waitForResponse cleanup funclet and of the Guard destructor.
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
    param([string]$Exe, [int]$MaxOffset, [switch]$All, [switch]$NoFault, [string]$Log)
    $arguments = @($MaxOffset)
    if ($All) { $arguments += 'all' }
    if ($NoFault) { $arguments += 'nofault' }
    & $Exe @arguments *> $Log
    "exit=$LASTEXITCODE" | Add-Content $Log
    return $LASTEXITCODE
}

function Read-ProbeFacts {
    param([string]$Log)
    $facts = [pscustomobject]@{
        offsets = @(); rejections = @(); guardEvents = @(); phases = @()
    }
    foreach ($line in Get-Content $Log) {
        if ($line -match '^P5C4B offset=(\d+) guards=(-?\d+)') { $facts.offsets += $line }
        elseif ($line -match '^P5C4B reject ') { $facts.rejections += $line }
        elseif ($line -match '^P5C4B trace: guard ') { $facts.guardEvents += $line }
        elseif ($line -match '^P5C4B (phase|stopped)') { $facts.phases += $line }
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
# Candidate fix shape, measured in the diagnostic copy only: the Lua error is
# raised from a helper frame that owns nothing, reached through a C++ call.
$probeHelper = Build-OomBinary -Name 'probe-ehsc-helper' `
    -Source "$root\tools\diagnostics\p5c4b_oom_probe.cpp" `
    -ExtraFlags @('/EHsc', '/DP5C4B_ERROR_HELPER')
$probeOutcome = Build-OomBinary -Name 'probe-ehsc-outcome' `
    -Source "$root\tools\diagnostics\p5c4b_oom_probe.cpp" `
    -ExtraFlags @('/EHsc', '/DP5C4B_OUTCOME_HELPER')
$probeBacktrace = Build-OomBinary -Name 'probe-ehsc-bt' `
    -Source "$root\tools\diagnostics\p5c4b_oom_probe.cpp" `
    -ExtraFlags @('/EHsc', '/DP5C4B_BACKTRACE')

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
$probeFacts.rejections | Add-Content (Join-Path $out 'probe-summary.txt')
$probeFacts.rejections | Set-Content (Join-Path $out 'probe-rejections.txt')

# Baseline of the same offset without the injected failure: the difference in
# the lease count is the product-side cost of the double destruction.
$baselineLog = Join-Path $out 'probe-baseline.log'
$baselineCode = Invoke-OomBinary -Exe $probe -MaxOffset 1 -NoFault -Log $baselineLog
"baseline_exit=$baselineCode" | Add-Content (Join-Path $out 'probe-summary.txt')
Select-String -Path $baselineLog -Pattern '^P5C4B offset' |
    ForEach-Object { $_.Line } | Add-Content (Join-Path $out 'probe-summary.txt')

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

# Candidate fix shape measured at the same offset: if the Lua error is raised
# from a helper frame that owns nothing, the double destruction must disappear
# while the injected failure still happens (rejected > 0, status != LUA_OK).
$helperLog = Join-Path $out 'probe-helper.log'
$helperCode = Invoke-OomBinary -Exe $probeHelper -MaxOffset 1 -Log $helperLog
@("variant=/DP5C4B_ERROR_HELPER with /EHsc (candidate fix shape, diagnostic copy)",
  "exit=$helperCode") | Set-Content (Join-Path $out 'probe-helper-summary.txt')
Select-String -Path $helperLog -Pattern '^P5C4B offset|^P5C4B reject|^exit=' |
    ForEach-Object { $_.Line } | Add-Content (Join-Path $out 'probe-helper-summary.txt')
$notes.Add("candidate helper shape at offset 1: exit=$helperCode")

# Second candidate: the whole protected body moves into a helper, so the frame
# that crosses the Lua boundary owns no automatic with a destructor.
$outcomeLog = Join-Path $out 'probe-outcome.log'
$outcomeCode = Invoke-OomBinary -Exe $probeOutcome -MaxOffset 1 -Log $outcomeLog
@("variant=/DP5C4B_OUTCOME_HELPER with /EHsc (P5C.4a shape, diagnostic copy)",
  "exit=$outcomeCode") | Set-Content (Join-Path $out 'probe-outcome-summary.txt')
Select-String -Path $outcomeLog -Pattern '^P5C4B offset|^P5C4B reject|^exit=' |
    ForEach-Object { $_.Line } | Add-Content (Join-Path $out 'probe-outcome-summary.txt')
$notes.Add("candidate outcome shape at offset 1: exit=$outcomeCode")

# Callers of the second destruction, captured in-process, then resolved with
# the PDB in a separate debugger session (no hardware breakpoints involved).
$btLog = Join-Path $out 'probe-backtrace.log'
$btCode = Invoke-OomBinary -Exe $probeBacktrace -MaxOffset 1 -Log $btLog
"exit=$btCode" | Add-Content $btLog
$btFrames = New-Object System.Collections.Generic.List[string]
foreach ($line in Get-Content $btLog) {
    if ($line -match '^P5C4B dtor-backtrace frames=(\d+)(.*)$') {
        foreach ($frame in ($Matches[2] -split '\s+' | Where-Object { $_ })) {
            $btFrames.Add($frame)
        }
    }
}
$btScript = Join-Path $out 'backtrace-commands.txt'
$btLines = New-Object System.Collections.Generic.List[string]
$btLines.Add('.reload')
$btIndex = 0
foreach ($frame in $btFrames) {
    $btIndex++
    $btLines.Add('.echo P5C4B-BT-' + $btIndex + ' ' + $frame)
    $btLines.Add('ln ' + $frame)
    $btLines.Add('u ' + $frame + ' L1')
}
$btLines.Add('q')
$btLines | Set-Content $btScript
& "$debuggers\cdb.exe" -o -y $symbolPath `
    -logo (Join-Path $out 'cdb-backtrace-debugger.log') `
    -cf $btScript $probeBacktrace 1 *> (Join-Path $out 'backtrace-resolved.txt')
$notes.Add("second-destruction backtrace frames=$($btFrames.Count)")

# --- 3. trace probe and /EHs probe: codegen and /EHc controls -------------
$traceLog = Join-Path $work 'trace-guard-1.log'
$traceCodes = @()
for ($iteration = 1; $iteration -le $RepeatProbe; $iteration++) {
    $traceLog = Join-Path $work "trace-guard-$iteration.log"
    $traceCodes += Invoke-OomBinary -Exe $probeTrace -MaxOffset 1 -Log $traceLog
}
Copy-Item (Join-Path $work 'trace-guard-1.log') (Join-Path $out 'trace-guard-first.log')
@("variant=/DP5C4B_TRACE with /EHsc (diagnostic control for the tracing)",
  "repeats=$($traceCodes.Count)",
  "exit_codes=$($traceCodes -join ',')") | Set-Content (Join-Path $out 'trace-guard-summary.txt')
$traceFacts = Read-ProbeFacts -Log (Join-Path $work 'trace-guard-1.log')
$traceFacts.guardEvents | Add-Content (Join-Path $out 'trace-guard-summary.txt')
$traceFacts.phases | Add-Content (Join-Path $out 'trace-guard-summary.txt')
$traceDtor = @($traceFacts.guardEvents | Where-Object { $_ -match 'guard dtor' }).Count
$traceCtor = @($traceFacts.guardEvents | Where-Object { $_ -match 'guard ctor' }).Count
@("guard_ctors=$traceCtor", "guard_dtors=$traceDtor") |
    Add-Content (Join-Path $out 'trace-guard-summary.txt')
@("trace_variant_guard_ctors=$traceCtor", "trace_variant_guard_dtors=$traceDtor") |
    Add-Content (Join-Path $out 'probe-summary.txt')

$standardLog = Join-Path $out 'probe-ehs-guard.log'
$standardCode = Invoke-OomBinary -Exe $probeStandard -MaxOffset 1 -Log $standardLog
@("variant=/EHs (diagnostic only: no /EHc assumption)",
  "exit=$standardCode") | Set-Content (Join-Path $out 'probe-ehs-summary.txt')
Select-String -Path $standardLog -Pattern '^P5C4B offset|^P5C4B reject|^exit=' |
    ForEach-Object { $_.Line } | Add-Content (Join-Path $out 'probe-ehs-summary.txt')
$standardRestLog = Join-Path $out 'probe-ehs-rest.log'
$standardRestCode = Invoke-OomBinary -Exe $probeStandard -MaxOffset 0 -All -Log $standardRestLog
"rest_exit=$standardRestCode" | Add-Content (Join-Path $out 'probe-ehs-summary.txt')

# --- 4. the remaining sweeps: the continuation path in isolation -----------
$restLog = Join-Path $out 'probe-rest.log'
$restCode = Invoke-OomBinary -Exe $probe -MaxOffset 0 -All -Log $restLog
@("probe=tools/diagnostics/p5c4b_oom_probe.cpp",
  "mode=0 all (guard sweep skipped)",
  "exit=$restCode") | Set-Content (Join-Path $out 'probe-rest-summary.txt')
Select-String -Path $restLog -Pattern '^P5C4B step|^P5C4B probe done|^Task |^Nested |^Targeted |^exit=' |
    ForEach-Object { $_.Line } | Add-Content (Join-Path $out 'probe-rest-summary.txt')
$notes.Add("remaining sweeps (task creation/execution/nested OOM): exit=$restCode")

# --- 5. isolation probe for the prepareManagedWait catch boundary ----------
$catch = Build-OomBinary -Name 'catch-boundary-ehsc' `
    -Source "$root\tools\diagnostics\p5c4b_catch_boundary_probe.cpp" -ExtraFlags @('/EHsc')
@("probe=tools/diagnostics/p5c4b_catch_boundary_probe.cpp",
  "flags=/O2 /Ob2 /DNDEBUG /MD /EHsc /DTDLUA_TESTING",
  "mode 0: control, no injected failure",
  "mode 1: lua_pushstring inside the catch, allocation fails",
  "mode 2: candidate fixed shape, push after the handler returned",
  "mode 3: prepareManagedWait shape (lease destroyed by the unwind), push in the catch",
  "mode 3 nofault: same shape without the injected failure",
  "mode 4: candidate fixed shape with the same lease") |
    Set-Content (Join-Path $out 'catch-boundary-summary.txt')
foreach ($catchRun in @(@(0), @(1), @(2), @(3), @(3, 'nofault'), @(4))) {
    $catchMode = $catchRun[0]
    $suffix = if ($catchRun.Count -gt 1) { "-$($catchRun[1])" } else { '' }
    $name = "catch-boundary-mode$catchMode$suffix"
    $log = Join-Path $out "$name.log"
    & $catch @catchRun *> $log
    "exit=$LASTEXITCODE" | Add-Content $log
    Select-String -Path $log -Pattern '^P5C4B catch' |
        ForEach-Object { $_.Line } | Add-Content (Join-Path $out 'catch-boundary-summary.txt')
}

# --- 6. cleanup funclets in the disassembly --------------------------------
function Export-Disassembly {
    param([string]$Name, [string]$Exe)
    $asm = Join-Path $out "disasm-$Name.txt"
    dumpbin.exe /nologo /disasm:nobytes $Exe | Out-File $asm
    # Every label (function symbol or funclet) is a non-instruction line: keep
    # the small list as provenance for the exact symbol names.
    $labels = New-Object System.Collections.Generic.List[string]
    foreach ($line in Get-Content $asm) {
        if ($line -notmatch '^\s*[0-9A-Fa-f]+:') { $labels.Add($line) }
    }
    $labels | Where-Object { $_ -match '\S' } | Set-Content (Join-Path $out "labels-$Name.txt")
    return $asm
}

function Export-Function {
    param([string]$Asm, [string]$Name, [string]$Destination)
    $collected = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in Get-Content $Asm) {
        if ($line -match '^\s*[0-9A-Fa-f]+:') {
            if ($inside) { $collected.Add($line) }
            continue
        }
        # A block starts at any label that names the function: the symbol
        # itself and, below it, each cleanup funclet the compiler emitted.
        if ($line -match [regex]::Escape($Name)) { $inside = $true; $collected.Add($line) }
        elseif ($inside) { $inside = $false }
    }
    $collected | Set-Content $Destination
    return $collected.Count
}

# Returns the module offsets of the cleanup funclets owned by $Name.
function Get-FuncletOffsets {
    param([string]$Asm, [string]$Name, [string]$Exe)
    $imageBase = 0
    foreach ($line in (dumpbin.exe /headers $Exe)) {
        if ($line -match '([0-9A-F]{8,})\s+image base') {
            $imageBase = [Convert]::ToInt64($Matches[1], 16)
            break
        }
    }
    $offsets = New-Object System.Collections.Generic.List[string]
    $entries = New-Object System.Collections.Generic.List[object]
    $label = $null
    foreach ($line in Get-Content $Asm) {
        if ($line -match '^\s*([0-9A-Fa-f]+):') {
            if ($label) {
                $address = [Convert]::ToInt64($Matches[1], 16)
                $offset = '0x' + ($address - $imageBase).ToString('x')
                $offsets.Add($offset)
                $tag = ($label -replace '.*::', '') -replace '[^0-9A-Za-z]', '_'
                $entries.Add([pscustomobject]@{ offset = $offset; label = $label.Trim(); tag = $tag })
                $label = $null
            }
            continue
        }
        if ($line -match [regex]::Escape($Name) -and $line -match '::`1''::') { $label = $line }
        else { $label = $null }
    }
    return [pscustomobject]@{ imageBase = $imageBase; offsets = $offsets; entries = $entries }
}

# Returns the module offsets of the addresses in $Asm's $Name block that call
# $Target: the exact Lua boundary calls of that function, as mapped by the PDB.
function Get-CallOffsets {
    param([string]$Asm, [string]$Name, [string]$Target, [Int64]$ImageBase)
    $offsets = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in Get-Content $Asm) {
        if ($line -match '^\s*([0-9A-Fa-f]+):') {
            if ($inside) {
                $candidate = [Convert]::ToInt64($Matches[1], 16)
                if ($line -match ('call\s+' + [regex]::Escape($Target))) {
                    $offsets.Add('0x' + ($candidate - $ImageBase).ToString('x'))
                }
            }
            continue
        }
        if ($line -match [regex]::Escape($Name)) { $inside = $true }
        elseif ($inside) { $inside = $false }
    }
    return $offsets
}

$summary = New-Object System.Collections.Generic.List[string]
foreach ($item in @(@('control-ehsc', $control), @('probe-ehsc', $probe),
                    @('probe-ehsc-trace', $probeTrace))) {
    $name = $item[0]
    $asm = Export-Disassembly -Name $name -Exe $item[1]
    foreach ($function in @('waitForResponse', 'managedWaitContinuation',
                            'managedWait', 'prepareManagedWait')) {
        $destination = Join-Path $out "func-$name-$function.txt"
        $count = Export-Function -Asm $asm -Name $function -Destination $destination
        $summary.Add("binary=$name function=$function lines=$count")
    }
}
$summary | Set-Content (Join-Path $out 'funclet-summary.txt')

foreach ($item in @(@('control-ehsc', $control), @('probe-ehsc', $probe))) {
    $name = $item[0]
    # Funclets are emitted after the function body and are interleaved with the
    # next function's labels, so they are collected from the whole disassembly.
    $asm = Join-Path $out "disasm-$name.txt"
    foreach ($function in @('waitForResponse', 'managedWaitContinuation')) {
        $file = Join-Path $out "func-$name-$function.txt"
        $destination = Join-Path $out "funclets-$name-$function.txt"
        $funclets = @(Select-String -Path $asm -CaseSensitive -Pattern '::`1''::' |
            Where-Object { $_.Line -match [regex]::Escape($function) })
        @("binary=$name function=$function funclets=$($funclets.Count)") | Set-Content $destination
        $funclets | ForEach-Object { $_.Line.Trim() } | Add-Content $destination
        @(Select-String -Path $file -Pattern 'lua_error|lua_yieldk|lua_settop|luaL_error') |
            ForEach-Object { $_.Line.Trim() } | Add-Content $destination
    }
}

# --- 7. CDB: the error path on the binary that reproduces ------------------
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

# The P5C4B_TRACE build does not reproduce the double destruction: the debugger
# therefore runs the plain probe build, the one that shares the control's flags
# while still printing the guard counters at the end of the offset.
$traceName = 'probe-ehsc'
# CDB rewrites '-' to '_' in module names, so the debugger expressions must use
# the underscored form while the file name keeps the dash.
$traceModule = 'probe_ehsc'
$traceExe = $work + '\' + "$traceName.exe"
$waitedFrame = Get-FuncletOffsets -Asm (Join-Path $out 'disasm-probe-ehsc.txt') `
    -Name 'waitForResponse' -Exe $traceExe
$continuationFrame = Get-FuncletOffsets -Asm (Join-Path $out 'disasm-probe-ehsc.txt') `
    -Name 'managedWaitContinuation' -Exe $traceExe
@("image_base=0x$($waitedFrame.imageBase.ToString('x'))",
  "waitForResponse_funclet_offsets=$($waitedFrame.offsets -join ',')",
  "managedWaitContinuation_funclet_offsets=$($continuationFrame.offsets -join ',')") |
    Add-Content (Join-Path $out 'funclet-summary.txt')

# Symbol discovery and PDB mapping: the exact names of the fixture's Guard
# methods, the nearest symbol and source line of every retained Lua boundary
# call, and the funclet addresses.
$symbolScript = Join-Path $out 'symbols-commands.txt'
$symbolLines = New-Object System.Collections.Generic.List[string]
$symbolLines.Add('.reload')
$symbolLines.Add('lm')
$symbolLines.Add('x ' + $traceModule + '!*Guard*')
$symbolLines.Add('x ' + $traceModule + '!*guard*')
$symbolLines.Add('x ' + $traceModule + '!*waitForResponse*')
$symbolLines.Add('x ' + $traceModule + '!tdlua::SchedulerCore::~SchedulerCore')
$symbolLines.Add('x ' + $traceModule + '!tdlua::SchedulerCore::SchedulerCore')
$symbolLines.Add('x lua54!luaD_throw')
$symbolLines.Add('x lua54!lua_error')
foreach ($pair in @(@('waitForResponse', 'lua_error'),
                    @('managedWaitContinuation', 'lua_error'),
                    @('managedWaitContinuation', '??0runtime_error@std@@'),
                    @('managedWait', 'lua_yieldk'),
                    @('managedWait', 'lua_error'))) {
    $function = $pair[0]
    $target = $pair[1]
    $calls = Get-CallOffsets -Asm (Join-Path $out 'disasm-probe-ehsc.txt') `
        -Name $function -Target $target -ImageBase $waitedFrame.imageBase
    foreach ($offset in $calls) {
        "call_offsets=$function/$target=$offset" | Add-Content (Join-Path $out 'funclet-summary.txt')
        $symbolLines.Add('ln ' + $traceModule + '+' + $offset)
        $symbolLines.Add('u ' + $traceModule + '+' + $offset + ' L1')
    }
}
foreach ($offset in $waitedFrame.offsets) {
    $symbolLines.Add('ln ' + $traceModule + '+' + $offset)
}
foreach ($offset in $continuationFrame.offsets) {
    $symbolLines.Add('ln ' + $traceModule + '+' + $offset)
}
$symbolLines.Add('q')
$symbolLines | Set-Content $symbolScript
$symbolLog = Join-Path $out 'cdb-symbols.log'
& "$debuggers\cdb.exe" -o -y $symbolPath `
    -logo (Join-Path $out 'cdb-symbols-debugger.log') `
    -cf $symbolScript $traceExe 1 *> $symbolLog
$symbols = @(Get-Content $symbolLog | Where-Object { $_ -match '!' }) |
    Where-Object { $_ -notmatch 'Couldn|error' }
$symbols | Set-Content (Join-Path $out 'cdb-symbols.txt')

$extraBreakpoints = New-Object System.Collections.Generic.List[string]
foreach ($line in Get-Content $symbolLog) {
    if ($line -match ([regex]::Escape($traceModule) + '!(\S*p5c4b_guard_(?:dtors|ctors))\s+=')) {
        # The counter is a data symbol: a write breakpoint on it reports the
        # exact stack of every construction and destruction of the fixture
        # guard, which is where the frame handler re-runs the cleanup.
        $symbol = $Matches[1]
        $tag = if ($symbol -match 'dtors') { 'GUARD-DTOR-WRITE' } else { 'GUARD-CTOR-WRITE' }
        $extraBreakpoints.Add('ba w4 ' + $traceModule + '!' + $symbol +
            ' ".echo P5C4B-' + $tag + '; kb 24; g"')
        "data_breakpoint=$tag symbol=$symbol" |
            Add-Content (Join-Path $out 'funclet-summary.txt')
    }
}
foreach ($line in Get-Content $symbolLog) {
    if ($line -match '!(\?\?1Guard@[^ \r\n]*)') {
        $extraBreakpoints.Add('bu ' + $traceModule + '!' + $Matches[1] +
            ' ".echo P5C4B-GUARD-DTOR; kb 20; g"')
    }
}
$funcletIndex = 0
foreach ($entry in $waitedFrame.entries) {
    $funcletIndex++
    $extraBreakpoints.Add('u ' + $traceModule + '+' + $entry.offset + ' L1')
    $extraBreakpoints.Add('bu ' + $traceModule + '+' + $entry.offset +
        ' ".echo P5C4B-WAITFRAME-FUNCLET-' + $funcletIndex + '-' + $entry.tag + '; kb 12; g"')
}
foreach ($entry in $continuationFrame.entries) {
    $funcletIndex++
    $extraBreakpoints.Add('u ' + $traceModule + '+' + $entry.offset + ' L1')
    $extraBreakpoints.Add('bu ' + $traceModule + '+' + $entry.offset +
        ' ".echo P5C4B-CONTINUATION-FUNCLET-' + $entry.tag + '; kb 12; g"')
}
foreach ($entry in $waitedFrame.entries) {
    "waitForResponse_funclet offset=$($entry.offset) label=$($entry.label)" |
        Add-Content (Join-Path $out 'funclet-summary.txt')
}
foreach ($entry in $continuationFrame.entries) {
    "managedWaitContinuation_funclet offset=$($entry.offset) label=$($entry.label)" |
        Add-Content (Join-Path $out 'funclet-summary.txt')
}

function Invoke-Trace {
    param([string]$Label, [int]$MaxOffset, [switch]$All, [switch]$UnwindDetail)
    $script = Join-Path $out "trace-commands-$Label.txt"
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('sxd av')
    $lines.Add('sxe 0xc0000374')
    $lines.Add('.reload')
    $lines.Add('bu lua54!luaD_throw ".echo P5C4B-RAISE-luaD_throw; kb 40; g"')
    $lines.Add('bu lua54!lua_error ".echo P5C4B-RAISE-lua_error; kb 30; g"')
    $lines.Add('bu lua54!luaL_error ".echo P5C4B-RAISE-luaL_error; kb 30; g"')
    $lines.Add('bu ' + $traceModule + '!tdlua::SchedulerCore::SchedulerCore ".echo P5C4B-CORE-CTOR; r @rcx; kb 12; g"')
    $lines.Add('bu ' + $traceModule + '!tdlua::SchedulerCore::~SchedulerCore ".echo P5C4B-CORE-DTOR; r @rcx; kb 12; g"')
    if ($UnwindDetail) {
        # Every frame the non-local jump walks through passes the C++ frame
        # handler: the stack there is the real sequence of abandoned frames.
        $lines.Add('bu VCRUNTIME140_1!__CxxFrameHandler4 ".echo P5C4B-FRAME-HANDLER; kb 25; g"')
        $lines.Add('bu VCRUNTIME140_1!__FrameHandler4::FrameUnwindToEmptyState ".echo P5C4B-UNWIND-EMPTY; kb 25; g"')
    }
    foreach ($breakpoint in $extraBreakpoints) { $lines.Add($breakpoint) }
    $lines.Add('bl')
    $lines.Add('g')
    $lines.Add('.echo ===END-OF-TRACE===')
    $lines.Add('bl')
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

Invoke-Trace -Label 'guard' -MaxOffset 1 -UnwindDetail
Invoke-Trace -Label 'rest' -MaxOffset 0 -All

# --- verdict --------------------------------------------------------------
# The raw disassemblies are derived evidence and unnecessarily large.
Remove-Item (Join-Path $out 'disasm-*.txt') -Force
$notes | Set-Content (Join-Path $out 'notes.txt')
'P5C.4b managed routing OOM forensics complete' |
    Set-Content (Join-Path $out 'result.txt')
