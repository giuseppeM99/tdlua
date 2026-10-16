$ErrorActionPreference = 'Stop'
# P5C.3 diagnostic driver. Diagnostic-only: it builds the isolated probes and the
# existing adapter test under a matrix of C++ exception-model flags, then captures
# the destruction sequence, allocation provenance and the disassembly of the
# tdlua functions. No TDLua source is modified.
$root = $env:GITHUB_WORKSPACE
$out = Join-Path $root '.ci\p5c3'
New-Item -ItemType Directory -Force $out | Out-Null
Start-Transcript -Path (Join-Path $out 'session.log')
try {
    & cl.exe /Bv 2>&1 | Out-File (Join-Path $out 'compiler.txt')
    git rev-parse HEAD | Out-File (Join-Path $out 'source.txt')

    $lua = Join-Path $out 'lua'
    & "$root\tools\windows\compile-lua-msvc.ps1" -WorkDirectory "$env:RUNNER_TEMP\lua-p5c3" -InstallDirectory $lua -DebugSymbols
    $env:PATH = "$lua;$env:PATH"

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
    if (-not (Test-Path "$debuggers\cdb.exe") -or -not (Test-Path "$debuggers\gflags.exe")) {
        throw 'CDB/GFlags unavailable on this runner'
    }

    # The two halves of /EHsc are separated so that the effect can be attributed
    # to /EHc rather than to /EHs. /EHa and the compiler default are controls.
    $matrix = [ordered]@{
        'EHsc'     = @('/EHsc')
        'EHs_EHc'  = @('/EHs', '/EHc')
        'EHs'      = @('/EHs')
        'EHs_EHc-' = @('/EHs', '/EHc-')
        'EHa'      = @('/EHa')
        'noEH'     = @()
    }
    $probes = @('p5c3_eh_matrix_probe', 'p5c3_eh_boundary_probe')

    Push-Location $out
    try {
        # --- 1. EH-model matrix of the isolated probes --------------------
        foreach ($name in $matrix.Keys) {
            $eh = @($matrix[$name])
            foreach ($probe in $probes) {
                $exe = "${probe}_$name.exe"
                $pdb = Join-Path $out "${probe}_$name.pdb"
                & cl.exe /nologo /MD @eh /Zi /O2 /DNDEBUG "/I$lua" "$root\tests\$probe.cpp" "/Fe:$exe" "/Fd:$pdb" `
                    /link "$lua\lua54.lib" /DEBUG /INCREMENTAL:NO "/PDB:$pdb" 2>&1 |
                    Out-File (Join-Path $out "$probe-$name-build.log")
                $build = $LASTEXITCODE
                "flags=$(($eh -join ' ')) build_exit=$build" |
                    Out-File -Append (Join-Path $out "$probe-$name-build.log")
                if ($build -ne 0) {
                    "$probe $name build_failed=$build" | Add-Content (Join-Path $out 'probe-exits.txt')
                    continue
                }
                & ".\$exe" *> (Join-Path $out "$probe-$name.log")
                "$probe $name exit=$LASTEXITCODE" | Add-Content (Join-Path $out 'probe-exits.txt')
            }
        }

        # --- 2. Adapter under every flag set -------------------------------
        foreach ($name in $matrix.Keys) {
            $eh = @($matrix[$name])
            $dir = Join-Path $out "adapter-$name"
            New-Item -ItemType Directory -Force $dir | Out-Null
            $pdb = Join-Path $dir 'tdlua_managed_adapter_test.pdb'
            & cl.exe /nologo /MD @eh /Zi /O2 /DNDEBUG /DTDLUA_TESTING `
                "/I$root\include" "/I$lua" "$root\tests\managed_adapter_test.cpp" `
                "/Fe:$dir\tdlua_managed_adapter_test.exe" "/Fd:$pdb" `
                /link "$lua\lua54.lib" /DEBUG /INCREMENTAL:NO "/PDB:$pdb" 2>&1 |
                Out-File (Join-Path $dir 'build.log')
            $build = $LASTEXITCODE
            "flags=$(($eh -join ' ')) build_exit=$build" |
                Out-File -Append (Join-Path $dir 'build.log')
            if ($build -ne 0) {
                "adapter $name build_failed=$build" | Add-Content (Join-Path $out 'adapter-exits.txt')
                continue
            }
            Get-FileHash "$dir\tdlua_managed_adapter_test.exe", "$lua\lua54.dll" |
                Format-List | Out-File (Join-Path $dir 'hashes.txt')
            1..3 | ForEach-Object {
                & "$dir\tdlua_managed_adapter_test.exe" --trace *> (Join-Path $dir "direct-$_.log")
                "adapter $name iteration=$_ exit=$LASTEXITCODE" | Add-Content (Join-Path $out 'adapter-exits.txt')
            }
        }

        # --- 3. Disassembly of the tdlua functions -------------------------
        # Text only, filtered to the managed-wait path, so the two builds can be
        # diffed without shipping a memory image.
        $disasm = Join-Path $out 'disasm'
        New-Item -ItemType Directory -Force $disasm | Out-Null
        foreach ($name in @('EHsc', 'EHs_EHc-')) {
            $binary = Join-Path $out "adapter-$name\tdlua_managed_adapter_test.exe"
            $raw = Join-Path $disasm "raw-$name.txt"
            & dumpbin.exe /nologo /disasm:nobytes $binary | Out-File $raw
            # Keep only tdlua / managed symbols and their instruction bodies.
            $keep = $false
            $lines = New-Object System.Collections.Generic.List[string]
            foreach ($line in Get-Content $raw) {
                if ($line -match '^\s*[0-9A-Fa-f]+:') {
                    if ($keep) { $lines.Add($line) }
                } else {
                    $keep = $line -match 'managed|Managed|SchedulerCore|RequestRouter|CoreAnchor|tdlua'
                    if ($keep) { $lines.Add("`n$line") }
                }
            }
            $lines | Set-Content (Join-Path $disasm "tdlua-$name.asm")
        }

        # --- 4. Allocation provenance for the first invalid access --------
        # Full Page Heap plus user stack traces. At the fault, !address shows the
        # state of the block that ~SchedulerCore dereferences, and !heap -p -a
        # would show its create/free stack where the extension is available.
        $target = 'tdlua_managed_adapter_test.exe'
        $bindir = Join-Path $out 'adapter-EHsc'
        $faultScript = @(
            'sxd av',
            'sxe 0xc0000374',
            'g',
            '.exr -1',
            'r',
            '.lastevent',
            'kw',
            '!analyze -v',
            '!address @rcx',
            '!heap -p -a @rcx',
            '!heap -p -a @rbp',
            'dt -b tdlua_managed_adapter_test!tdlua::SchedulerCore @rcx',
            'q'
        )
        $faultScript | Set-Content (Join-Path $out 'fault-commands.txt')
        try {
            & "$debuggers\gflags.exe" /p /enable $target /full *> (Join-Path $out 'pageheap-enable.log')
            if ($LASTEXITCODE -ne 0) { throw 'page heap enable failed' }
            & "$debuggers\gflags.exe" /p *> (Join-Path $out 'pageheap-status.log')
            if (-not (Select-String -Path (Join-Path $out 'pageheap-status.log') -Pattern $target)) {
                throw 'Page heap configuration not confirmed'
            }
            & "$debuggers\gflags.exe" /i $target +ust *> (Join-Path $out 'ust-enable.log')
            Push-Location $bindir
            try {
                & "$debuggers\cdb.exe" -o -logo (Join-Path $out 'pageheap-debugger.log') `
                    -cf (Join-Path $out 'fault-commands.txt') .\tdlua_managed_adapter_test.exe --trace *> (Join-Path $out 'pageheap-console.log')
                "pageheap_cdb_exit=$LASTEXITCODE" | Add-Content (Join-Path $out 'adapter-exits.txt')
            }
            finally { Pop-Location }
        }
        finally {
            & "$debuggers\gflags.exe" /i $target -ust *> (Join-Path $out 'ust-disable.log')
            & "$debuggers\gflags.exe" /p /disable $target *> (Join-Path $out 'pageheap-disable.log')
            & "$debuggers\gflags.exe" /p *> (Join-Path $out 'pageheap-after.log')
        }

        # --- 5. Destruction sequence, per flag set -------------------------
        # Every SchedulerCore construction and destruction is logged with its
        # caller stack, so that the same object address appearing twice, and the
        # frame that runs the destructor, are directly visible.
        $traceScript = @(
            'sxd av',
            'bu tdlua_managed_adapter_test!tdlua::SchedulerCore::SchedulerCore "r @rcx; g"',
            'bu tdlua_managed_adapter_test!tdlua::SchedulerCore::~SchedulerCore "r @rcx; kb 12; g"',
            'g',
            '.echo ===END-OF-TRACE===',
            'q'
        )
        $traceScript | Set-Content (Join-Path $out 'trace-commands.txt')
        foreach ($name in @('EHsc', 'EHs', 'EHs_EHc-')) {
            $dir = Join-Path $out "adapter-$name"
            if (-not (Test-Path (Join-Path $dir $target))) { continue }
            Push-Location $dir
            try {
                & "$debuggers\cdb.exe" -o -logo (Join-Path $out "trace-$name-debugger.log") `
                    -cf (Join-Path $out 'trace-commands.txt') ".\$target" --trace *> (Join-Path $out "trace-$name-console.log")
                "trace $name cdb_exit=$LASTEXITCODE" | Add-Content (Join-Path $out 'adapter-exits.txt')
            }
            finally { Pop-Location }
        }
    }
    finally { Pop-Location }
}
finally { Stop-Transcript }
