# P5C.4a validation harness. Validation only: it does not build or change any
# product behavior. It proves, on the original MSVC Release configuration
# (/O2 /MD /EHsc) that reproduced the P5C.3 crash, that the managed wait yield
# frame no longer owns the SchedulerCore lease:
#   1. the adapter is rebuilt with /EHsc and repeated 100 times;
#   2. the cleanup funclets attributed to the frame that calls lua_yieldk are
#      inspected in the disassembly (and must be gone, while the non-inlined
#      helper owns the lease cleanup);
#   3. every SchedulerCore construction/destruction is traced under the
#      debugger, with and without full page heap, and the sequence must
#      alternate with no destruction inside a non-local jump.
param(
    [Parameter(Mandatory = $true)][string]$LuaDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$Iterations = 100
)
$ErrorActionPreference = 'Stop'
$root = $env:GITHUB_WORKSPACE
$out = $OutputDirectory
$work = Join-Path $env:RUNNER_TEMP 'p5c4-managed-wait'
New-Item -ItemType Directory -Force $out, $work | Out-Null
$lua = $LuaDirectory
$target = 'tdlua_managed_adapter_test.exe'
$exe = Join-Path $work $target
$pdb = Join-Path $work 'tdlua_managed_adapter_test.pdb'
$env:PATH = "$lua;$work;$env:PATH"

# --- 1. Rebuild the adapter with the original Release flags ----------------
& cl.exe /Bv 2>&1 | Out-File (Join-Path $out 'compiler.txt')
(& git rev-parse HEAD | Out-String).Trim() | Out-File (Join-Path $out 'source.txt')
& cl.exe /nologo /MD /EHsc /Zi /O2 /Ob2 /DNDEBUG /DTDLUA_TESTING `
    "/I$root\include" "/I$lua" "$root\tests\managed_adapter_test.cpp" `
    "/Fe:$exe" "/Fd:$pdb" `
    /link "$lua\lua54.lib" /DEBUG /INCREMENTAL:NO "/PDB:$pdb" 2>&1 |
    Tee-Object -FilePath (Join-Path $out 'adapter-build.log')
if ($LASTEXITCODE -ne 0) { throw "adapter build failed: $LASTEXITCODE" }
'flags=/O2 /Ob2 /DNDEBUG /MD /EHsc' | Add-Content (Join-Path $out 'adapter-build.log')
dumpbin.exe /dependents $exe | Out-File (Join-Path $out 'adapter-dependencies.log')
Get-FileHash $exe, (Join-Path $lua 'lua54.dll') | Format-List |
    Out-File (Join-Path $out 'hashes.txt')

# --- 2. Repeat the configuration that used to crash ------------------------
$codes = @()
for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
    & $exe --trace *> (Join-Path $work "direct-$iteration.log")
    $codes += $LASTEXITCODE
}
$passed = @($codes | Where-Object { $_ -eq 0 }).Count
@("requested=$Iterations",
  "iterations_run=$($codes.Count)",
  "passed=$passed",
  "failed=$($codes.Count - $passed)",
  "exit_codes=$($codes -join ',')") |
    Set-Content (Join-Path $out 'adapter-repeat-summary.txt')
Copy-Item (Join-Path $work 'direct-1.log') (Join-Path $out 'adapter-trace-first.log')
$owners = @()
foreach ($file in Get-ChildItem (Join-Path $work 'direct-*.log')) {
    foreach ($text in Get-Content $file) {
        if ($text -match 'before response core=([0-9A-Fa-f]+) owners=([0-9]+)') {
            $owners += $Matches[2]
        }
    }
}
$ownerBuckets = @($owners | Sort-Object -Unique)
"observations=$($owners.Count)" | Set-Content (Join-Path $out 'adapter-owners-histogram.txt')
$owners | Group-Object | ForEach-Object { "owners=$($_.Name) occurrences=$($_.Count)" } |
    Add-Content (Join-Path $out 'adapter-owners-histogram.txt')
if ($codes.Count -ne $Iterations -or $passed -ne $Iterations) {
    throw "managed adapter failed $($codes.Count - $passed) of $Iterations iterations under /O2 /MD /EHsc"
}
if ($ownerBuckets.Count -ne 1) {
    throw "owner count is not stable across repeats: $($ownerBuckets -join ',')"
}

# --- 3. Cleanup funclets of the frame that calls lua_yieldk ----------------
dumpbin.exe /nologo /disasm:nobytes $exe | Out-File (Join-Path $out 'raw-disasm.txt')
$keep = $false
$lines = New-Object System.Collections.Generic.List[string]
foreach ($line in Get-Content (Join-Path $out 'raw-disasm.txt')) {
    if ($line -match '^\s*[0-9A-Fa-f]+:') {
        if ($keep) { $lines.Add($line) }
    } else {
        $keep = $line -match 'managed|Managed|SchedulerCore|RequestRouter|CoreAnchor|tdlua'
        if ($keep) { $lines.Add("`n$line") }
    }
}
$asm = Join-Path $out 'tdlua.asm'
$lines | Set-Content $asm
if (-not (Select-String -Path $asm -CaseSensitive -Pattern '\?managedWait@tdlua@@YAH')) {
    throw 'managedWait is absent from the disassembly'
}
if (-not (Select-String -Path $asm -Pattern 'lua_yieldk')) {
    throw 'lua_yieldk is absent from the disassembly'
}
$yieldFrameFunclets = @(Select-String -Path $asm -CaseSensitive -Pattern "managedWait'::")
$helperFunclets = @(Select-String -Path $asm -CaseSensitive -Pattern "prepareManagedWait'::")
@("yield_frame_cleanup_funclets=$($yieldFrameFunclets.Count)",
  "prepare_helper_funclets=$($helperFunclets.Count)") |
    Set-Content (Join-Path $out 'funclet-summary.txt')
if ($yieldFrameFunclets.Count -eq 0) {
    'none' | Set-Content (Join-Path $out 'yield-frame-funclets.txt')
}
else {
    $yieldFrameFunclets | ForEach-Object { $_.Line.Trim() } |
        Set-Content (Join-Path $out 'yield-frame-funclets.txt')
}
$helperFunclets | ForEach-Object { $_.Line.Trim() } |
    Set-Content (Join-Path $out 'prepare-helper-funclets.txt')
# MSVC names every cleanup funclet after the function that owns it, so an empty
# result here means the frame that calls lua_yieldk has no destructor to run
# during a non-local jump.
if ($yieldFrameFunclets.Count -ne 0) {
    throw "cleanup funclet still attributed to the yield frame: $($yieldFrameFunclets[0].Line.Trim())"
}
if ($helperFunclets.Count -eq 0) {
    throw 'prepareManagedWait has no cleanup funclet: the lease may have been folded into the yield frame'
}

# --- 4. SchedulerCore lifetime trace --------------------------------------
$debuggers = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64'
if (-not (Test-Path "$debuggers\cdb.exe") -or -not (Test-Path "$debuggers\gflags.exe")) {
    $installer = "$env:RUNNER_TEMP\winsdk-debuggers.exe"
    Invoke-WebRequest 'https://go.microsoft.com/fwlink/?linkid=2120843' -OutFile $installer
    $signature = Get-AuthenticodeSignature $installer
    $signature | Format-List | Out-File (Join-Path $out 'installer-signature.txt')
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft') {
        throw 'Invalid debugger installer signature'
    }
    $install = Start-Process $installer -ArgumentList '/features OptionId.WindowsDesktopDebuggers /quiet /norestart' -Wait -PassThru
    "debuggers installer exit=$($install.ExitCode)" | Out-File (Join-Path $out 'debuggers-install.txt')
    if ($install.ExitCode -notin 0, 3010) { throw 'Debugger-only installation failed' }
}
if (-not (Test-Path "$debuggers\cdb.exe")) { throw 'CDB unavailable' }

$traceScript = Join-Path $out 'trace-commands.txt'
@('sxd av',
  'sxe 0xc0000374',
  'bu tdlua_managed_adapter_test!tdlua::SchedulerCore::SchedulerCore ".echo P5C4-CTOR; r @rcx; g"',
  'bu tdlua_managed_adapter_test!tdlua::SchedulerCore::~SchedulerCore ".echo P5C4-DTOR; r @rcx; kb 12; g"',
  'g',
  '.echo ===END-OF-TRACE===',
  'q') | Set-Content $traceScript

function Invoke-ManagedTrace([string]$label) {
    $log = Join-Path $out "trace-$label-console.log"
    Push-Location $work
    try {
        & "$debuggers\cdb.exe" -o -logo (Join-Path $out "trace-$label-debugger.log") `
            -cf $traceScript ".\$target" --trace *> $log
        "cdb_exit=$LASTEXITCODE" | Add-Content $log
    }
    finally { Pop-Location }
    return $log
}

function Test-ManagedLifetime([string]$log, [string]$label) {
    $events = @()
    $pending = $null
    $lines = @(Get-Content $log)
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $line = $lines[$index]
        if ($line -match '^P5C4-(CTOR|DTOR)$') { $pending = $Matches[1]; continue }
        if ($pending -and $line -match '^rcx=([0-9a-fA-F]+)') {
            $stack = if ($pending -eq 'DTOR') { $lines[$index..([Math]::Min($index + 14, $lines.Count - 1))] -join "`n" } else { '' }
            $events += [pscustomobject]@{ kind = $pending; address = $Matches[1].ToLowerInvariant(); stack = $stack }
            $pending = $null
        }
    }
    $live = @{}
    $violations = @()
    $unwrapDestructions = @()
    foreach ($event in $events) {
        if ($event.kind -eq 'CTOR') {
            if ($live.ContainsKey($event.address)) { $violations += "second construction of live core $($event.address)" }
            $live[$event.address] = $true
        }
        else {
            if (-not $live.ContainsKey($event.address)) {
                $violations += "destruction of core $($event.address) with no live construction (double release or use after free)"
            }
            else { $live.Remove($event.address) }
            if ($event.stack -match 'lua_yieldk|luaD_throw|__longjmp_internal|lua_error') {
                $unwrapDestructions += $event.address
            }
        }
    }
    $summary = @("trace=$label",
                 "constructions=$(@($events | Where-Object { $_.kind -eq 'CTOR' }).Count)",
                 "destructions=$(@($events | Where-Object { $_.kind -eq 'DTOR' }).Count)",
                 "cores_alive_at_end=$($live.Count)",
                 "destructions_inside_lua_jump=$($unwrapDestructions.Count)",
                 "lifetime_violations=$($violations.Count)")
    $summary += ($violations | ForEach-Object { "violation=$_" })
    $summary | Set-Content (Join-Path $out "trace-$label-lifetime.txt")
    if ($violations.Count -ne 0) {
        throw "${label}: SchedulerCore lifetime violations: $($violations -join '; ')"
    }
    if ($unwrapDestructions.Count -ne 0) {
        throw "${label}: SchedulerCore destroyed while a Lua non-local jump unwinds: $($unwrapDestructions -join ', ')"
    }
    if ($live.Count -ne 0) {
        throw "${label}: $($live.Count) SchedulerCore objects are still alive at exit"
    }
    return $events
}

# Without page heap: the process runs the traced scenario normally.
$plainLog = Invoke-ManagedTrace 'pageheap-off'
if (-not (Select-String -Path $plainLog -Pattern '===END-OF-TRACE===')) {
    throw 'traced run did not reach the end of the scenario'
}
Test-ManagedLifetime $plainLog 'pageheap-off' | Out-Null

# With full page heap: a released block becomes PAGE_NOACCESS, so a use after
# free faults at the exact instruction instead of corrupting the heap.
& "$debuggers\gflags.exe" /p /enable $target /full *> (Join-Path $out 'pageheap-enable.log')
try {
    if ($LASTEXITCODE -ne 0) { throw 'Could not enable full page heap' }
    & "$debuggers\gflags.exe" /p *> (Join-Path $out 'pageheap-status.log')
    if (-not (Select-String -Path (Join-Path $out 'pageheap-status.log') -Pattern $target)) {
        throw 'Page heap configuration not confirmed'
    }
    $pageHeapLog = Invoke-ManagedTrace 'pageheap-on'
    if (Select-String -Path $pageHeapLog -Pattern 'Access violation|Critical error detected|RtlReportCriticalFailure') {
        throw 'page heap run reported a fault'
    }
    Test-ManagedLifetime $pageHeapLog 'pageheap-on' | Out-Null
}
finally {
    & "$debuggers\gflags.exe" /p /disable $target *> (Join-Path $out 'pageheap-disable.log')
    & "$debuggers\gflags.exe" /p *> (Join-Path $out 'pageheap-after.log')
}

'P5C.4a managed wait unwind evidence complete' | Set-Content (Join-Path $out 'result.txt')
