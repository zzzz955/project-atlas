<#
.SYNOPSIS
    Local clang-tidy pre-filter (architecture-design.md §15.5i, cpp-style.md §7.3).

.DESCRIPTION
    THIS IS NOT A GATE. The gate is clang-tidy on linux-ci (.github/workflows/ci.yml). This
    script runs the VS-bundled clang-tidy over the MSVC compile database so that naming / bugprone
    / modernize violations can be seen BEFORE a push, and nothing more. Its exit code is always 0
    for that reason: a clean sweep here is not evidence, and treating it as one is the mistake this
    header exists to prevent.

    The two toolchains disagree in BOTH directions, measured 2026-08-13:
      - CI sees what this cannot: bugprone-unchecked-optional-access never fires under MSVC's STL,
        which is exactly the check that reddened CI on 2026-08-13 (§15.5i).
      - This sees what CI cannot: server/atlas/core/crash.cpp is a Windows-only translation unit on
        this side, and MSVC's headers produce bugprone-exception-escape reports that libstdc++ does
        not. Those findings are real for this platform but the CI gate will never grade them.

    ci-gate.ps1 skips clang-tidy because CMake hands it an MSVC /Yc PCH it cannot read. This script
    works around that by stripping the PCH flags from a COPY of compile_commands.json - never the
    real one, which the compiler uses.

.EXAMPLE
    powershell -NoProfile -File server\scripts\tidy-prefilter.ps1
    powershell -NoProfile -File server\scripts\tidy-prefilter.ps1 -Filter db_runtime
#>
param(
    [string]$Preset = 'windows-ci',
    # Substring match on the source path - run one file instead of the whole tree.
    [string]$Filter = ''
)

$ErrorActionPreference = 'Stop'

$ServerDir = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $ServerDir "build\$Preset"
$Database = Join-Path $BuildDir 'compile_commands.json'

if (-not (Test-Path -LiteralPath $Database)) {
    throw "No compile database at $Database - run 'cmake --preset $Preset' from server/ first."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found - is Visual Studio 2022 installed?' }
$install = & $vswhere -latest -products * -property installationPath
$tidy = Join-Path $install 'VC\Tools\Llvm\x64\bin\clang-tidy.exe'
if (-not (Test-Path -LiteralPath $tidy)) { throw "clang-tidy not found at $tidy" }

# The stripped database goes to a temp directory. Rewriting the real one would hand the next
# build a compile line the compiler never agreed to.
$work = Join-Path ([System.IO.Path]::GetTempPath()) 'atlas-tidy-prefilter'
New-Item -ItemType Directory -Force -Path $work | Out-Null

$entries = @()
foreach ($entry in (Get-Content -LiteralPath $Database -Raw | ConvertFrom-Json)) {
    # cmake_pch.cxx is the /Yc translation unit itself - it exists only to bake the PCH.
    if ($entry.file -match 'cmake_pch\.cxx$') { continue }
    $command = $entry.command -replace '/Yu\S*', '' -replace '/Fp\S*', '' -replace '/FIcmake_pch\S*', ''
    $entries += [pscustomobject]@{ directory = $entry.directory; command = $command; file = $entry.file }
}
$entries | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $work 'compile_commands.json') -Encoding utf8

# The same set the CI step walks: the hand-written C++ only, never generated output.
$sources = $entries.file |
    Where-Object { $_ -notmatch '[/\\]generated[/\\]' -and $_ -notmatch '[/\\]build[/\\]' } |
    Where-Object { $Filter -eq '' -or $_ -like "*$Filter*" }

Write-Host "[tidy] $($sources.Count) translation units, clang-tidy from $install" -ForegroundColor Cyan
Write-Host '[tidy] advisory only - the gate is clang-tidy on linux-ci (architecture-design.md 15.5i)' -ForegroundColor Yellow

# Native stderr is an ERROR RECORD under 'Stop' - clang-tidy's harmless "N warnings generated"
# summary would abort the sweep. The diagnostics themselves come back on stdout.
$ErrorActionPreference = 'Continue'

$started = Get-Date
$findings = 0
foreach ($source in $sources) {
    $output = & $tidy -p $work --quiet $source 2>$null
    if ($output) {
        $findings += ($output | Select-String -Pattern 'warning: |error: ').Count
        $output | Write-Host
    }
}

$elapsed = [int]((Get-Date) - $started).TotalSeconds
Write-Host "[tidy] $findings diagnostic line(s) in ${elapsed}s" -ForegroundColor Cyan
Write-Host '[tidy] a clean sweep here is NOT a green CI (15.5i / cpp-style.md 7.3)' -ForegroundColor Yellow
exit 0
