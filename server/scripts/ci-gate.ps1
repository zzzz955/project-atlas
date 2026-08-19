<#
.SYNOPSIS
    project-atlas CI gate (architecture-design.md §15.4, cpp-style.md §7.3).

.DESCRIPTION
    gen:check -> core-purity -> format-check -> clang-tidy -> build(unity ON) -> build(unity OFF)
    -> test

    The repository has no git remote yet, so this script - not .github/workflows/ci.yml - is the
    gate that is actually exercised. Keep the two in step.

    One step is NOT equivalent between the two: clang-tidy is skipped here and enforced only on
    linux-ci in the workflow, because clang-tidy cannot read the MSVC PCH that the local Ninja/cl
    tree produces. The skip is announced, not silent - see the step for the measurement.

    The unity-OFF build is the step that catches missing #includes and ODR collisions
    (architecture-design.md §15.1, cost 4). Never drop it to save time.

    The gate loads server/.env (key names logged, values never - §5.4) so the DB-backed suites
    actually run, and it FAILS if a test was Skipped while a database was reachable. ctest exits 0
    on a skip, so without this the gate printed PASS having verified nothing about the DB axis.

    Configure runs as a prep step before format/tidy because clang-tidy needs the
    compile_commands.json that only a configured Ninja build tree produces.

.PARAMETER WhatIf
    Print the plan without running anything. Used as a syntax/wiring smoke test.

.EXAMPLE
    powershell -NoProfile -File server\scripts\ci-gate.ps1
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$UnityOnPreset = 'windows-debug',
    [string]$UnityOffPreset = 'windows-ci'
)

$ErrorActionPreference = 'Stop'

$ServerDir = Split-Path -Parent $PSScriptRoot
$RepoRoot = Split-Path -Parent $ServerDir
$StepNumber = 0

function Invoke-Step {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [scriptblock]$Action
    )

    $script:StepNumber++
    if ($WhatIfPreference) {
        Write-Host ("[gate] {0}. {1}  (WhatIf - not executed)" -f $script:StepNumber, $Name)
        return
    }

    Write-Host ("[gate] {0}. {1}" -f $script:StepNumber, $Name) -ForegroundColor Cyan
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "[gate] FAILED at step $($script:StepNumber): $Name (exit $LASTEXITCODE)"
    }
}

function Import-VsDevEnv {
    # cmake / ninja / cl / clang-* ship with VS 2022 and are not on PATH by default.
    if (Get-Command cmake -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found - is Visual Studio 2022 installed?' }

    $install = & $vswhere -latest -products * -property installationPath
    if (-not $install) { throw 'No Visual Studio 2022 installation found.' }

    $devcmd = Join-Path $install 'Common7\Tools\VsDevCmd.bat'
    cmd /c "call `"$devcmd`" -arch=amd64 -host_arch=amd64 -no_logo && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }

    $env:PATH = (Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin') + ';' +
                (Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' +
                (Join-Path $install 'VC\Tools\Llvm\x64\bin') + ';' + $env:PATH
}

function Import-DotEnv {
    # The DB integration suites (db_runtime_test / tx_atomicity_test / game_equip_test) read
    # ATLAS_DB_* from the environment and GTEST_SKIP() when it is absent. Without this the gate
    # printed [gate] PASS while every DB test was Skipped - a green run that proved nothing, which
    # is exactly what those suites' own comments say is worse than a red one. Measured 2026-08-10.
    param([Parameter(Mandatory)] [string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Host "[gate] no $Path - DB-backed tests will skip (see the test step)" -ForegroundColor Yellow
        return @()
    }

    $loaded = @()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$') { continue }
        $name = $Matches[1]
        $value = $Matches[2].Trim()
        if ($value -eq '') { continue }
        # An explicitly exported variable outranks the file - that is how a caller points the gate
        # at a different database without editing .env.
        if (Test-Path -LiteralPath "env:$name") { continue }
        Set-Item -Path "env:$name" -Value $value
        $loaded += $name
    }

    # Key names only, never values (architecture-design.md §5.4). .env holds credentials.
    if ($loaded) { Write-Host ("[gate] .env loaded (key names only): {0}" -f ($loaded -join ', ')) }

    # .env carries ATLAS_DB_HOST=mysql, the compose SERVICE name, which only resolves on the
    # compose network. This gate always runs on the host, where that name is nothing. Overriding
    # only a file-sourced value keeps an explicit caller-set host intact.
    if ($loaded -contains 'ATLAS_DB_HOST' -and $env:ATLAS_DB_HOST -eq 'mysql') {
        $env:ATLAS_DB_HOST = '127.0.0.1'
        Write-Host '[gate] ATLAS_DB_HOST: compose service name -> 127.0.0.1 (gate runs on the host)'
    }

    return $loaded
}

function Test-DbReachable {
    # Decides whether a Skipped DB test is acceptable or a gate failure. A TCP connect is enough:
    # the question is "was a database available and we tested nothing anyway", not "can we log in".
    if (-not $env:ATLAS_DB_HOST) { return $false }
    $port = if ($env:ATLAS_DB_PORT) { [UInt16]$env:ATLAS_DB_PORT } else { [UInt16]3306 }

    $client = New-Object System.Net.Sockets.TcpClient
    try {
        return $client.ConnectAsync($env:ATLAS_DB_HOST, $port).Wait(2000) -and $client.Connected
    } catch {
        return $false
    } finally {
        $client.Dispose()
    }
}

if (-not $WhatIfPreference) { Import-VsDevEnv }

$dbReachable = $false
if (-not $WhatIfPreference) {
    [void](Import-DotEnv -Path (Join-Path $ServerDir '.env'))
    $dbReachable = Test-DbReachable
    if ($dbReachable) {
        Write-Host ("[gate] database reachable at {0}:{1} - DB-backed tests must run" -f $env:ATLAS_DB_HOST, $env:ATLAS_DB_PORT)
    }
}

# Both elements go inside one @(): `@(a), (b)` builds a NESTED array (element 0 is itself an
# array), which Get-ChildItem -Path rejects with "Cannot convert 'System.Object[]' to ... 'String'".
# `game` belongs here too. It was missing when server/game/ landed, so an entire new source tree
# was outside format-check while the gate still said PASS - the same shape of hole as a Skipped test
# counted as a pass. Every new top-level source tree must be added here in the change that creates it.
$sourceGlobs = @((Join-Path $ServerDir 'atlas'), (Join-Path $ServerDir 'game'), (Join-Path $ServerDir 'loadgen'), (Join-Path $ServerDir 'console_client'), (Join-Path $ServerDir 'tests'), (Join-Path $ServerDir 'generated'))
$buildDirUnityOff = Join-Path $ServerDir "build/$UnityOffPreset"

# generated/ IS in $sourceGlobs, and that is a change from the original design. The generators now
# pipe every emitted .h/.cpp through the repo .clang-format themselves (tools/cxx-format.js), so the
# old rationale - "generated output is never formatted because it is never hand-edited" - no longer
# holds. Two files under generated/ were also never generated at all (the hand-written
# generated/{db,pkt}/tests/*_test.cpp), and they sat outside format-check for exactly as long as the
# blanket exclusion stood. clang-tidy stays excluded (.clang-tidy ExcludeHeaderFilterRegex).
# cpp-style.md §7.1.

Invoke-Step 'gen:check (generated-output drift)' {
    Push-Location $RepoRoot
    try { npm run gen:check } finally { Pop-Location }
}

# Mechanical enforcement of "the core must not know the game" (architecture-design.md §14, §15.4).
# Implemented once in Node (tools/core_purity/) so this gate and .github/workflows/ci.yml run the
# same checker - two transcriptions of the same rule drift apart silently.
Invoke-Step 'core-purity (core must not know the game)' {
    Push-Location $RepoRoot
    try { npm run check:core-purity } finally { Pop-Location }
}

Invoke-Step "configure ($UnityOnPreset, $UnityOffPreset)" {
    Push-Location $ServerDir
    try {
        cmake --preset $UnityOnPreset
        if ($LASTEXITCODE -ne 0) { return }
        cmake --preset $UnityOffPreset
    } finally { Pop-Location }
}

Invoke-Step 'format-check' {
    $files = Get-ChildItem -Path $sourceGlobs -Recurse -File -Include *.h, *.cpp |
             Select-Object -ExpandProperty FullName
    if (-not $files) { Write-Host '[gate] no sources to format'; $global:LASTEXITCODE = 0; return }
    clang-format --style=file --dry-run -Werror @files
}

# clang-tidy is CI-only (Linux/clang), NOT part of this local gate. Measured 2026-08-06:
# clang-tidy 19.1.5 aborts on the MSVC compile_commands.json because CMake's PCH step hands it
# cmake_pch.cxx.pch, an MSVC /Yc artifact that clang cannot read:
#     error: file '.../cmake_pch.cxx.pch' is not a valid precompiled PCH file:
#            file doesn't start with AST file magic [clang-diagnostic-error]
# Everything else in the MSVC database parses fine (with the PCH suppressed, the same invocation
# exits 0 under --warnings-as-errors=*), so the incompatibility is PCH-only. Passing /Y- behind
# clang-tidy's back would tidy a translation unit the compiler never actually builds, so the step
# is demoted rather than patched. .github/workflows/ci.yml keeps clang-tidy on linux-ci, where
# clang produces the compile database and the PCH it consumes. cpp-style.md §7.3.
# Undo condition: local clang-cl builds (a clang-cl preset would emit a clang-readable PCH).
Invoke-Step 'clang-tidy (SKIPPED on Windows/MSVC - runs in CI on linux-ci)' {
    Write-Host '[gate] clang-tidy skipped: MSVC PCH is unreadable by clang-tidy. See the comment above this step.' -ForegroundColor Yellow
    $global:LASTEXITCODE = 0
}

Invoke-Step "build (unity ON - $UnityOnPreset)" {
    Push-Location $ServerDir
    try { cmake --build --preset $UnityOnPreset } finally { Pop-Location }
}

Invoke-Step "build (unity OFF - $UnityOffPreset, missing-include / ODR gate)" {
    Push-Location $ServerDir
    try { cmake --build --preset $UnityOffPreset } finally { Pop-Location }
}

Invoke-Step "test (ctest, $UnityOffPreset)" {
    Push-Location $ServerDir
    try {
        $output = & ctest --preset $UnityOffPreset --output-on-failure 2>&1
        $output | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { return }

        # ctest exits 0 on a Skipped test, so the exit code alone cannot tell "everything passed"
        # from "the interesting half never ran". Whether that is acceptable depends entirely on
        # whether a database was there to be tested against.
        $skipped = @($output | Select-String -SimpleMatch '***Skipped').Count
        if ($skipped -eq 0) { return }

        if ($dbReachable) {
            Write-Host ("[gate] {0} test(s) Skipped while {1}:{2} was reachable." -f `
                        $skipped, $env:ATLAS_DB_HOST, $env:ATLAS_DB_PORT) -ForegroundColor Red
            Write-Host '[gate] A database was available and the suite tested nothing against it - that is a gate failure, not a pass.' -ForegroundColor Red
            $global:LASTEXITCODE = 1
            return
        }

        Write-Host ("[gate] {0} test(s) Skipped: no database reachable at {1}." -f `
                    $skipped, $env:ATLAS_DB_HOST) -ForegroundColor Yellow
        Write-Host '[gate] The DB axis was NOT verified by this run. Start it and re-run:' -ForegroundColor Yellow
        Write-Host '[gate]     docker compose --env-file server/.env up -d mysql' -ForegroundColor Yellow
    } finally { Pop-Location }
}

if (-not $WhatIfPreference) { Write-Host '[gate] PASS' -ForegroundColor Green }
