$ErrorActionPreference = 'Stop'
$root = $env:GITHUB_WORKSPACE
$out = Join-Path $root '.ci\memory-forensics'
New-Item -ItemType Directory -Force $out | Out-Null
Start-Transcript -Path (Join-Path $out 'session.log')
try {
    & cl.exe /Bv 2>&1 | Out-File (Join-Path $out 'compiler.txt')
    git rev-parse HEAD | Out-File (Join-Path $out 'source.txt')
    $lua = Join-Path $out 'lua'
    & "$root\tools\windows\compile-lua-msvc.ps1" -WorkDirectory "$env:RUNNER_TEMP\lua-forensics" -InstallDirectory $lua -DebugSymbols
    $env:PATH = "$lua;$env:PATH"
    $debuggers = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64'
    if (-not (Test-Path "$debuggers\cdb.exe")) {
        $installer = "$env:RUNNER_TEMP\winsdk-debuggers.exe"
        Invoke-WebRequest 'https://go.microsoft.com/fwlink/?linkid=2120843' -OutFile $installer
        $signature = Get-AuthenticodeSignature $installer
        $signature | Format-List | Out-File (Join-Path $out 'installer-signature.txt')
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft') { throw 'Invalid debugger installer signature' }
        $install = Start-Process $installer -ArgumentList '/features OptionId.WindowsDesktopDebuggers /quiet /norestart' -Wait -PassThru
        "debuggers installer exit=$($install.ExitCode)" | Out-File (Join-Path $out 'debuggers-install.txt')
        if ($install.ExitCode -notin 0,3010) { throw 'Debugger-only installation failed' }
    }
    if (-not (Test-Path "$debuggers\cdb.exe") -or -not (Test-Path "$debuggers\gflags.exe")) { throw 'CDB/GFlags unavailable after debugger-only installation' }
    foreach ($mode in @('ehs', 'release')) {
        $dir = Join-Path $out $mode
        New-Item -ItemType Directory -Force $dir | Out-Null
        Push-Location $dir
        try {
            $opt = if ($mode -eq 'debug') { @('/Od', '/Ob0') } else { @('/O2', '/Ob2', '/DNDEBUG') }
            $link = @('/DEBUG', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF')
            if ($mode -eq 'noicf') { $link = @('/DEBUG', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:NOICF') }
            [string[]]$eh = if ($mode -eq 'ehs') { '/EHs', '/EHc-' } else { '/EHsc' }
            & cl.exe /nologo /MD @eh /Zi @opt /DTDLUA_TESTING "/I$root\include" "/I$lua" "$root\tests\managed_adapter_test.cpp" /Fe:tdlua_managed_adapter_test.exe /link "$lua\lua54.lib" @link 2>&1 | Tee-Object build.log
            if ($LASTEXITCODE -ne 0) { throw "Adapter build failed: $mode" }
            dumpbin.exe /dependents tdlua_managed_adapter_test.exe | Out-File dependencies.log
            Get-FileHash tdlua_managed_adapter_test.exe,"$lua\lua54.dll" | Format-List | Out-File hashes.txt
            1..3 | ForEach-Object {
                & .\tdlua_managed_adapter_test.exe --trace *> "direct-$_.log"
                "iteration=$_ exit=$LASTEXITCODE" | Add-Content exits.txt
            }
            foreach ($source in @('windows_lua_unwind_probe', 'core_anchor_ownership_test', 'managed_routing_oom_test')) {
                & cl.exe /nologo /MD @eh /Zi @opt /DTDLUA_TESTING "/I$root\include" "/I$lua" "$root\tests\$source.cpp" "/Fe:$source.exe" /link "$lua\lua54.lib" @link *> "$source-build.log"
                if ($LASTEXITCODE -ne 0) { throw "Diagnostic build failed: $source" }
                & ".\$source.exe" *> "$source.log"
                "$source exit=$LASTEXITCODE" | Add-Content exits.txt
            }
            # Stop at the first AV/heap failure. Do not publish process memory.
            $commands = 'sxe av; sxe 0xc0000374; g; .exr -1; r; u @rip-10 @rip+20; kv; ~* kv; !ext.heap -p -a @rax; !ext.heap -p -a @rcx; !ext.heap -p -a @rdx; !ext.heap -p -a @r8; !ext.heap -p -a @r9; lm v; q'
            & "$debuggers\cdb.exe" -o -G -logo debugger.log -c $commands .\tdlua_managed_adapter_test.exe --trace *> debugger-console.log
            "cdb_exit=$LASTEXITCODE" | Add-Content exits.txt
            try {
                & "$debuggers\gflags.exe" /p /enable tdlua_managed_adapter_test.exe /full *> pageheap-enable.log
                if ($LASTEXITCODE -ne 0) { throw 'Could not enable page heap' }
                & "$debuggers\gflags.exe" /p *> pageheap-status.log
                if (-not (Select-String -Path pageheap-status.log -Pattern 'tdlua_managed_adapter_test')) { throw 'Page heap configuration not confirmed' }
                & "$debuggers\cdb.exe" -o -G -logo pageheap-debugger.log -c $commands .\tdlua_managed_adapter_test.exe --trace *> pageheap-console.log
                "pageheap_cdb_exit=$LASTEXITCODE" | Add-Content exits.txt
            } finally {
                & "$debuggers\gflags.exe" /p /disable tdlua_managed_adapter_test.exe *> pageheap-disable.log
                if ($LASTEXITCODE -ne 0) { throw 'Could not disable page heap' }
                & "$debuggers\gflags.exe" /p *> pageheap-after.log
            }
        } finally { Pop-Location }
    }
} finally { Stop-Transcript }
