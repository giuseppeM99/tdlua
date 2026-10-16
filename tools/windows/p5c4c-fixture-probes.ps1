# P5C.4c diagnostic harness. Temporary: it must be dropped before any merge.
#
# It measures, on the original MSVC Release configuration (/O2 /Ob2 /DNDEBUG
# /MD, Lua 5.4.8 built with debug symbols):
#   1. the real, uninstrumented fixture tests/managed_routing_oom_test.cpp run in
#      isolated processes, with its exit code and its summary;
#   2. the same fixture plus measurement counters (tools/diagnostics/
#      p5c4c_oom_probe.cpp) at the failing offset, in the whole 1..80 sweep and
#      without injection, built with /EHsc and with /EHs;
#   3. the same instrumented source compiled with -DP5C4C_PREFIX_INLINE, which
#      restores the pre-fix b2c47d8 frame shape, as the same-run reference for
#      the defect;
#   4. the cleanup funclets the compiler emits for waitForResponse (fixed) and
#      for the pre-fix shape, from the disassembly;
#   5. the managedWaitContinuation error path (Gate B1) and the waitById yield
#      boundary (Gate B2), each built with both /EHsc and /EHs, with the
#      SchedulerCore and ManagedState owner counts around the resume.
#
# It changes no product code and no test assertion, and it never turns a failing
# measurement into a pass: the expectations it enforces are exactly the ones the
# task asks for, and every raw observation stays in the output directory.
param(
    [Parameter(Mandatory = $true)][string]$LuaDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$Iterations = 3
)
$ErrorActionPreference = 'Stop'
$root = $env:GITHUB_WORKSPACE
$out = $OutputDirectory
$work = Join-Path $env:RUNNER_TEMP 'p5c4c-probes'
New-Item -ItemType Directory -Force $out, $work | Out-Null
$lua = $LuaDirectory
$env:PATH = "$lua;$work;$env:PATH"
$checks = New-Object System.Collections.Generic.List[object]

trap {
    @("message=$($_.Exception.Message)",
      "position=$($_.InvocationInfo.PositionMessage)",
      "stack=$($_.ScriptStackTrace)") | Set-Content (Join-Path $out 'harness-error.txt')
    throw
}

function Add-Check {
    param([string]$Name, [bool]$Passed, [string]$Detail)
    $checks.Add([pscustomobject]@{ name = $Name; passed = $Passed; detail = $Detail })
}

function Build-Binary {
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

function Invoke-Binary {
    param([string]$Exe, [string[]]$Arguments = @(), [string]$Log)
    & $Exe @Arguments *> $Log
    $code = $LASTEXITCODE
    "exit=$code" | Add-Content $Log
    return $code
}

# The last line the probe printed for one offset.
function Get-Observation {
    param([string]$Log, [string]$Pattern = '^P5C4C offset=')
    $found = @(Select-String -Path $Log -Pattern $Pattern)
    if ($found.Count -eq 0) { return $null }
    return $found[-1].Line.Trim()
}

function Get-Field {
    param([string]$Line, [string]$Field)
    if (-not $Line) { return '' }
    $match = [regex]::Match($Line, "$Field=(\S+)")
    if (-not $match.Success) { return '' }
    return $match.Groups[1].Value
}

& cl.exe /Bv 2>&1 | Out-File (Join-Path $out 'compiler.txt')
(& git rev-parse HEAD | Out-String).Trim() | Set-Content (Join-Path $out 'source.txt')
(& git rev-parse --abbrev-ref HEAD | Out-String).Trim() | Set-Content (Join-Path $out 'branch.txt')
(& git status --porcelain | Out-String).Trim() | Set-Content (Join-Path $out 'worktree-status.txt')
Get-FileHash (Join-Path $lua 'lua54.dll'), (Join-Path $lua 'lua54.lib') |
    Format-List | Out-File (Join-Path $out 'lua-hashes.txt')

# --- 1. builds -------------------------------------------------------------
$realFixture = Build-Binary -Name 'fixture-ehsc' -Source "$root\tests\managed_routing_oom_test.cpp"
$fixedEhsc = Build-Binary -Name 'probe-ehsc-fixed' -Source "$root\tools\diagnostics\p5c4c_oom_probe.cpp"
$fixedEhs = Build-Binary -Name 'probe-ehs-fixed' -Source "$root\tools\diagnostics\p5c4c_oom_probe.cpp" -ExtraFlags @('/EHs')
$prefixEhsc = Build-Binary -Name 'probe-ehsc-prefix' -Source "$root\tools\diagnostics\p5c4c_oom_probe.cpp" -ExtraFlags @('/DP5C4C_PREFIX_INLINE')
$prefixEhs = Build-Binary -Name 'probe-ehs-prefix' -Source "$root\tools\diagnostics\p5c4c_oom_probe.cpp" -ExtraFlags @('/EHs', '/DP5C4C_PREFIX_INLINE')
$b1Ehsc = Build-Binary -Name 'b1-ehsc' -Source "$root\tools\diagnostics\p5c4c_continuation_probe.cpp"
$b1Ehs = Build-Binary -Name 'b1-ehs' -Source "$root\tools\diagnostics\p5c4c_continuation_probe.cpp" -ExtraFlags @('/EHs')
$b2Ehsc = Build-Binary -Name 'b2-ehsc' -Source "$root\tools\diagnostics\p5c4c_waitbyid_probe.cpp"
$b2Ehs = Build-Binary -Name 'b2-ehs' -Source "$root\tools\diagnostics\p5c4c_waitbyid_probe.cpp" -ExtraFlags @('/EHs')

# --- 2. the real fixture, isolated processes -------------------------------
$fixtureLog = Join-Path $out 'fixture-ehsc-runs.log'
if (Test-Path $fixtureLog) { Remove-Item $fixtureLog -Force }
$fixtureCodes = @()
for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
    "=== fixture iteration $iteration/$Iterations ===" | Add-Content $fixtureLog
    $fixtureCodes += Invoke-Binary -Exe $realFixture -Log $fixtureLog
}
$fixturePassed = @($fixtureCodes | Where-Object { $_ -eq 0 }).Count
@("iterations=$Iterations",
  "passed=$fixturePassed",
  "exit_codes=$($fixtureCodes -join ',')") | Set-Content (Join-Path $out 'fixture-ehsc-summary.txt')
Add-Check -Name 'real-fixture-ehsc-exit' -Passed ($fixturePassed -eq $Iterations) `
    -Detail "codes=$($fixtureCodes -join ',')"

# --- 3. the instrumented fixture: offset 1, nofault and the whole sweep -----
$variants = @(
    @{ Name = 'fixed-ehsc'; Exe = $fixedEhsc },
    @{ Name = 'fixed-ehs'; Exe = $fixedEhs },
    @{ Name = 'prefix-ehsc'; Exe = $prefixEhsc },
    @{ Name = 'prefix-ehs'; Exe = $prefixEhs }
)
$offsetLines = New-Object System.Collections.Generic.List[string]
foreach ($variant in $variants) {
    $name = $variant.Name
    foreach ($run in @(
        @{ Tag = 'offset1-inject'; Arguments = @('1') },
        @{ Tag = 'offset1-nofault'; Arguments = @('1', 'nofault') },
        @{ Tag = 'sweep'; Arguments = @() }
    )) {
        $log = Join-Path $out "probe-$name-$($run.Tag).log"
        $code = Invoke-Binary -Exe $variant.Exe -Arguments $run.Arguments -Log $log
        $line = Get-Observation -Log $log
        $offsetLines.Add("variant=$name run=$($run.Tag) exit=$code $(if ($line) { $line } else { '(no observation)' })")
    }
}
$offsetLines | Set-Content (Join-Path $out 'probe-offset-summary.txt')

# The task's A3 invariants, on the committed (fixed) shape only.
foreach ($name in @('fixed-ehsc', 'fixed-ehs')) {
    $log = Join-Path $out "probe-$name-offset1-inject.log"
    $line = Get-Observation -Log $log
    $exit = (Get-Field -Line ((Get-Content $log | Where-Object { $_ -like 'exit=*' } | Select-Object -Last 1)) -Field 'exit')
    $guards = Get-Field -Line $line -Field 'guards'
    $ctors = Get-Field -Line $line -Field 'guard_ctors_here'
    $dtors = Get-Field -Line $line -Field 'guard_dtors_here'
    $rejected = Get-Field -Line $line -Field 'rejected'
    $task = Get-Field -Line $line -Field 'task_status'
    $status = Get-Field -Line $line -Field 'status'
    $ok = ($exit -eq '0') -and ($guards -eq '0') -and ($ctors -eq '1') -and
          ($dtors -eq '1') -and ($rejected -eq '2') -and ($status -eq '4') -and
          ($task -eq '4')
    Add-Check -Name "$name-offset1" -Passed $ok `
        -Detail "exit=$exit guards=$guards ctors=$ctors dtors=$dtors rejected=$rejected status=$status task_status=$task"
    $sweepLog = Join-Path $out "probe-$name-sweep.log"
    $sweepExit = (Get-Content $sweepLog | Where-Object { $_ -like 'exit=*' } | Select-Object -Last 1) -replace 'exit=', ''
    Add-Check -Name "$name-sweep" -Passed ($sweepExit -eq '0') -Detail "exit=$sweepExit"
}

# --- 3b. the real fixture, b2c47d8 (pre-fix) against the committed file -----
#
# The instrumented copy above is a rewrite of the fixture, so its code
# generation is not the fixture's. These four binaries are the real fixture:
# the committed file and the byte materialisation of its b2c47d8 baseline, each
# built with /EHsc and with /EHs. The two guard-counting copies differ from the
# real file by four textual substitutions only (a ctor counter, a dtor counter,
# their two globals and one extra field in the existing diagnostic line), so a
# clustered skip/double destruction can be told apart in the real frame shape.
function New-GuardCountingFixture {
    param([string]$Source, [string]$Destination)
    $text = [System.IO.File]::ReadAllText($Source)
    $replacements = @(
        @{ From = 'explicit Guard(int &value) : count(value) { ++count; }'
           To = 'explicit Guard(int &value) : count(value) { ++count; ++p5c4c_guard_ctors; }' },
        @{ From = '~Guard() { --count; }'
           To = '~Guard() { --count; ++p5c4c_guard_dtors; }' },
        @{ From = 'Scenario *current = nullptr;'
           To = "int p5c4c_guard_ctors = 0;`nint p5c4c_guard_dtors = 0;`nScenario *current = nullptr;" },
        @{ From = '<< "guard diagnostic: offset=" << offset'
           To = '<< "guard diagnostic: ctors=" << p5c4c_guard_ctors << " dtors=" << p5c4c_guard_dtors << " offset=" << offset' }
    )
    foreach ($replacement in $replacements) {
        $hits = ([regex]::Matches($text, [regex]::Escape($replacement.From))).Count
        if ($hits -ne 1) {
            throw "guard-counting transform matched $hits times (expected 1): $($replacement.From)"
        }
        $text = $text.Replace($replacement.From, $replacement.To)
    }
    [System.IO.File]::WriteAllText($Destination, $text)
    return $Destination
}

$prefixSource = Join-Path $work 'fixture-b2c47d8.cpp'
$prefixLines = @(& git -C $root show 'b2c47d8:tests/managed_routing_oom_test.cpp')
if ($LASTEXITCODE -ne 0) { throw 'could not materialise the b2c47d8 fixture' }
$prefixText = ($prefixLines -join "`n") + "`n"
[System.IO.File]::WriteAllText($prefixSource, $prefixText)
$prefixBlobSize = (& git -C $root cat-file -s 'b2c47d8:tests/managed_routing_oom_test.cpp' | Out-String).Trim()
$prefixSize = (Get-Item $prefixSource).Length
$prefixHash = (Get-FileHash -Algorithm SHA256 $prefixSource).Hash
$expectedPrefixHash = '95D6DD914B224E40EB4E30FD204C19E78FA5C1D69ACFC75337F5C93E7D8E3478'
@("baseline_commit=b2c47d8",
  "baseline_blob_bytes=$prefixBlobSize",
  "materialised_bytes=$prefixSize",
  "materialised_sha256=$prefixHash",
  "expected_sha256=$expectedPrefixHash",
  "materialised_has_pre_fix_shape=$($prefixText -notmatch 'performWait' -and $prefixText -match 'int waitForResponse\(lua_State \*L\)')",
  "committed_has_post_fix_shape=$(([System.IO.File]::ReadAllText((Join-Path $root 'tests\managed_routing_oom_test.cpp'))) -match 'performWait')",
  "baseline_guard_message_present=$($prefixText -match 'Lua error skipped a C\+\+ destructor')") |
    Set-Content (Join-Path $out 'fixture-provenance.txt')
Add-Check -Name 'baseline-fixture-provenance' `
    -Passed (($prefixHash -eq $expectedPrefixHash) -and
             ($prefixText -notmatch 'performWait') -and
             ($prefixText -match 'Lua error skipped a C\+\+ destructor')) `
    -Detail "sha256=$prefixHash bytes=$prefixSize blob=$prefixBlobSize"

$postSource = "$root\tests\managed_routing_oom_test.cpp"
$guardPost = New-GuardCountingFixture -Source $postSource `
    -Destination (Join-Path $work 'fixture-guard-post.cpp')
$guardPrefix = New-GuardCountingFixture -Source $prefixSource `
    -Destination (Join-Path $work 'fixture-guard-prefix.cpp')

$comparison = New-Object System.Collections.Generic.List[string]
$comparison.Add("post_fixture=tests/managed_routing_oom_test.cpp (committed, Gate A2 shape)")
$comparison.Add("prefix_fixture=b2c47d8:tests/managed_routing_oom_test.cpp")
foreach ($item in @(
    @{ Name = 'post-ehsc'; Source = $postSource; Flags = @() },
    @{ Name = 'post-ehs'; Source = $postSource; Flags = @('/EHs') },
    @{ Name = 'prefix-ehsc'; Source = $prefixSource; Flags = @() },
    @{ Name = 'prefix-ehs'; Source = $prefixSource; Flags = @('/EHs') },
    @{ Name = 'guard-post-ehsc'; Source = $guardPost; Flags = @() },
    @{ Name = 'guard-post-ehs'; Source = $guardPost; Flags = @('/EHs') },
    @{ Name = 'guard-prefix-ehsc'; Source = $guardPrefix; Flags = @() },
    @{ Name = 'guard-prefix-ehs'; Source = $guardPrefix; Flags = @('/EHs') }
)) {
    $exe = Build-Binary -Name "fixture-$($item.Name)" -Source $item.Source -ExtraFlags $item.Flags
    $log = Join-Path $out "fixture-$($item.Name)-runs.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    $codes = @()
    for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
        "=== $($item.Name) iteration $iteration/$Iterations ===" | Add-Content $log
        $codes += Invoke-Binary -Exe $exe -Log $log
    }
    $comparison.Add("variant=$($item.Name) exit_codes=$($codes -join ',')")
    foreach ($line in @(Select-String -Path $log -Pattern 'diagnostic|skipped a C\+\+ destructor|^[0-9]+ offsets passed')) {
        $comparison.Add("  $($item.Name): $($line.Line.Trim())")
    }
    if ($item.Name -in @('post-ehs', 'prefix-ehs', 'guard-post-ehs', 'guard-prefix-ehs')) {
        $passed = @($codes | Where-Object { $_ -eq 0 }).Count
        Add-Check -Name "fixture-$($item.Name)-exit" -Passed ($passed -eq $Iterations) `
            -Detail "codes=$($codes -join ',')"
    }
}
$comparison | Set-Content (Join-Path $out 'fixture-comparison.txt')

# --- 4. cleanup funclets in the disassembly --------------------------------
function Export-Disassembly {
    param([string]$Name, [string]$Exe)
    $asm = Join-Path $out "disasm-$Name.txt"
    dumpbin.exe /nologo /disasm:nobytes $Exe | Out-File $asm
    return $asm
}

$funcletSummary = New-Object System.Collections.Generic.List[string]
foreach ($item in @(
    @{ Name = 'fixed-ehsc'; Exe = $fixedEhsc },
    @{ Name = 'prefix-ehsc'; Exe = $prefixEhsc },
    @{ Name = 'b1-ehsc'; Exe = $b1Ehsc },
    @{ Name = 'b2-ehsc'; Exe = $b2Ehsc }
)) {
    $asm = Export-Disassembly -Name $item.Name -Exe $item.Exe
    foreach ($function in @('waitForResponse', 'performWait', 'managedWaitContinuation', 'waitById')) {
        # Funclets are emitted after the function body and their labels carry the
        # owner name and the `1' scope marker, so they are counted in the whole
        # disassembly.
        $funclets = @(Select-String -Path $asm -CaseSensitive -Pattern '::`1''::' |
            Where-Object { $_.Line -match [regex]::Escape($function) })
        $funcletSummary.Add("binary=$($item.Name) function=$function funclets_emitted=$($funclets.Count)")
        $funclets | ForEach-Object { $funcletSummary.Add("  " + $_.Line.Trim()) }
    }
    foreach ($call in @(Select-String -Path $asm -CaseSensitive -Pattern 'call.*lua_error|lua_yieldk')) {
        $funcletSummary.Add("binary=$($item.Name) lua_boundary_call=" + $call.Line.Trim())
    }
}
$funcletSummary | Set-Content (Join-Path $out 'funclet-summary.txt')
$fixedWaitForResponse = @(Select-String -Path (Join-Path $out 'funclet-summary.txt') `
    -Pattern 'binary=fixed-ehsc function=waitForResponse funclets_emitted=(\d+)')
if ($fixedWaitForResponse.Count -eq 1) {
    $count = [int]([regex]::Match($fixedWaitForResponse[0].Line, '=(\d+)$').Groups[1].Value)
    Add-Check -Name 'fixed-waitForResponse-owns-no-funclet' -Passed ($count -eq 0) `
        -Detail "funclets_emitted=$count"
} else {
    Add-Check -Name 'fixed-waitForResponse-owns-no-funclet' -Passed $false -Detail 'measurement missing'
}

# --- 5. Gate B1: managedWaitContinuation error path -------------------------
$b1Summary = New-Object System.Collections.Generic.List[string]
foreach ($item in @(@{ Name = 'ehsc'; Exe = $b1Ehsc }, @{ Name = 'ehs'; Exe = $b1Ehs })) {
    foreach ($run in @(
        @{ Tag = 'failed'; Arguments = @('5') },
        @{ Tag = 'oom1'; Arguments = @('1', 'oom', '1') },
        @{ Tag = 'oom2'; Arguments = @('1', 'oom', '2') },
        @{ Tag = 'oom3'; Arguments = @('1', 'oom', '3') },
        @{ Tag = 'oom4'; Arguments = @('1', 'oom', '4') }
    )) {
        $log = Join-Path $out "b1-$($item.Name)-$($run.Tag).log"
        $code = Invoke-Binary -Exe $item.Exe -Arguments $run.Arguments -Log $log
        $observations = @(Select-String -Path $log -Pattern '^B1 mode=')
        foreach ($observation in $observations) {
            $b1Summary.Add("flags=$($item.Name) run=$($run.Tag) exit=$code $($observation.Line.Trim())")
        }
        if ($observations.Count -eq 0) {
            $b1Summary.Add("flags=$($item.Name) run=$($run.Tag) exit=$code (no observation)")
        }
    }
}
$b1Summary | Set-Content (Join-Path $out 'b1-summary.txt')
foreach ($item in @('ehsc', 'ehs')) {
    $codes = @()
    foreach ($tag in @('failed', 'oom1', 'oom2', 'oom3', 'oom4')) {
        $log = Join-Path $out "b1-$item-$tag.log"
        $codes += (Get-Content $log | Where-Object { $_ -like 'exit=*' } | Select-Object -Last 1) -replace 'exit=', ''
    }
    $passed = @($codes | Where-Object { $_ -eq '0' }).Count
    Add-Check -Name "b1-$item-exit" -Passed ($passed -eq $codes.Count) -Detail "codes=$($codes -join ',')"
}

# --- 6. Gate B2: waitById yield boundary -----------------------------------
$b2Summary = New-Object System.Collections.Generic.List[string]
foreach ($item in @(@{ Name = 'ehsc'; Exe = $b2Ehsc }, @{ Name = 'ehs'; Exe = $b2Ehs })) {
    foreach ($run in @(
        @{ Tag = 'success'; Arguments = @('success', '5') },
        @{ Tag = 'timed'; Arguments = @('timed', '5') },
        @{ Tag = 'failed'; Arguments = @('failed', '5') },
        @{ Tag = 'oom1'; Arguments = @('oom', '1', '1') },
        @{ Tag = 'oom2'; Arguments = @('oom', '1', '2') },
        @{ Tag = 'oom3'; Arguments = @('oom', '1', '3') }
    )) {
        $log = Join-Path $out "b2-$($item.Name)-$($run.Tag).log"
        $code = Invoke-Binary -Exe $item.Exe -Arguments $run.Arguments -Log $log
        $observations = @(Select-String -Path $log -Pattern '^B2 mode=')
        foreach ($observation in $observations) {
            $b2Summary.Add("flags=$($item.Name) run=$($run.Tag) exit=$code $($observation.Line.Trim())")
        }
        if ($observations.Count -eq 0) {
            $b2Summary.Add("flags=$($item.Name) run=$($run.Tag) exit=$code (no observation)")
        }
    }
}
$b2Summary | Set-Content (Join-Path $out 'b2-summary.txt')
foreach ($item in @('ehsc', 'ehs')) {
    $codes = @()
    foreach ($tag in @('success', 'timed', 'failed', 'oom1', 'oom2', 'oom3')) {
        $log = Join-Path $out "b2-$item-$tag.log"
        $codes += (Get-Content $log | Where-Object { $_ -like 'exit=*' } | Select-Object -Last 1) -replace 'exit=', ''
    }
    $passed = @($codes | Where-Object { $_ -eq '0' }).Count
    Add-Check -Name "b2-$item-exit" -Passed ($passed -eq $codes.Count) -Detail "codes=$($codes -join ',')"
}

# --- 7. result -------------------------------------------------------------
$checks | Format-Table -AutoSize | Out-String -Width 200 | Set-Content (Join-Path $out 'checks.txt')
$failures = @($checks | Where-Object { -not $_.passed })
@("checks=$($checks.Count)", "failures=$($failures.Count)") | Set-Content (Join-Path $out 'result.txt')
$failures | ForEach-Object { "FAILED $($_.name): $($_.detail)" } | Add-Content (Join-Path $out 'result.txt')
if ($failures.Count -ne 0) {
    throw "$($failures.Count) P5C.4c probe checks failed; see result.txt"
}
