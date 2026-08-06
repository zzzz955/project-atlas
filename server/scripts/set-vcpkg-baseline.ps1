<#
.SYNOPSIS
    Writes the vcpkg builtin-baseline SHA into server/vcpkg.json.

.DESCRIPTION
    setup.bat calls this right after it clones vcpkg. The manifest ships with an EMPTY baseline on
    purpose: nobody may invent a SHA by hand, it has to be the commit that was actually cloned.
    Idempotent — re-running with the same SHA rewrites nothing.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$ManifestPath,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9a-f]{40}$')] [string]$Sha
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ManifestPath)) {
    throw "vcpkg manifest not found: $ManifestPath"
}

$text = [IO.File]::ReadAllText($ManifestPath)
$pattern = '("builtin-baseline"\s*:\s*")[0-9a-fA-F]*(")'

if ($text -notmatch $pattern) {
    throw "No 'builtin-baseline' key in $ManifestPath - refusing to guess where it belongs."
}

$updated = [Text.RegularExpressions.Regex]::Replace($text, $pattern, ('${1}' + $Sha + '${2}'))

if ($updated -eq $text) {
    Write-Host "[baseline] already pinned to $Sha"
    return
}

# LF + no BOM, per .editorconfig / .gitattributes.
[IO.File]::WriteAllText($ManifestPath, ($updated -replace "`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Host "[baseline] pinned builtin-baseline to $Sha"
