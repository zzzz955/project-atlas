<#
.SYNOPSIS
    project-atlas CI gate (architecture-design.md §15.4, cpp-style.md §7.3).

.DESCRIPTION
    gen:check -> format-check -> clang-tidy -> build(unity ON) -> build(unity OFF) -> test

    The repository has no git remote yet, so this script - not .github/workflows/ci.yml - is the
    gate that is actually exercised. Keep the two in step.

    🔴 One step is NOT equivalent between the two: clang-tidy is skipped here and enforced only on
    linux-ci in the workflow, because clang-tidy cannot read the MSVC PCH that the local Ninja/cl
    tree produces. The skip is announced, not silent - see the step for the measurement.

    🔴 The unity-OFF build is the step that catches missing #includes and ODR collisions
    (architecture-design.md §15.1, cost 4). Never drop it to save time.

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

if (-not $WhatIfPreference) { Import-VsDevEnv }

# 🔴 Both elements go inside one @(): `@(a), (b)` builds a NESTED array (element 0 is itself an
# array), which Get-ChildItem -Path rejects with "Cannot convert 'System.Object[]' to ... 'String'".
$sourceGlobs = @((Join-Path $ServerDir 'atlas'), (Join-Path $ServerDir 'tests'))
$buildDirUnityOff = Join-Path $ServerDir "build/$UnityOffPreset"

# 🔴 generated/ is deliberately absent from $sourceGlobs: generated output is never formatted or
# tidied (cpp-style.md §7.1) because it is never hand-edited.

Invoke-Step 'gen:check (generated-output drift)' {
    Push-Location $RepoRoot
    try { npm run gen:check } finally { Pop-Location }
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

# 🔴 clang-tidy is CI-only (Linux/clang), NOT part of this local gate. Measured 2026-08-06:
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
    try { ctest --preset $UnityOffPreset --output-on-failure } finally { Pop-Location }
}

if (-not $WhatIfPreference) { Write-Host '[gate] PASS' -ForegroundColor Green }
