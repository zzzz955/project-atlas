param(
    [Parameter(Mandatory = $true)]
    [string]$Dump,

    [Parameter(Mandatory = $true)]
    [string]$Binary,

    [Parameter(Mandatory = $true)]
    [string]$Symbols
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingPath([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Label does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

$dumpPath = Resolve-ExistingPath $Dump 'Dump'
$binaryPath = Resolve-ExistingPath $Binary 'Binary'
$symbolPath = Resolve-ExistingPath $Symbols 'Symbols'

if ($IsWindows -or $env:OS -eq 'Windows_NT') {
    if ([IO.Path]::GetExtension($dumpPath) -ne '.dmp') {
        throw 'Windows symbolization expects a .dmp file.'
    }

    $debugger = Get-Command cdb.exe -ErrorAction SilentlyContinue
    $debuggerPath = if ($null -ne $debugger) {
        $debugger.Source
    } else {
        $sdkDebugger = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Debuggers\x64\cdb.exe'
        if (Test-Path -LiteralPath $sdkDebugger) { $sdkDebugger } else { $null }
    }
    if ($null -eq $debuggerPath) {
        throw 'cdb.exe was not found. Install Debugging Tools for Windows from the Windows SDK.'
    }

    $pdb = if ((Get-Item -LiteralPath $symbolPath).PSIsContainer) {
        Join-Path $symbolPath (([IO.Path]::GetFileNameWithoutExtension($binaryPath)) + '.pdb')
    } else {
        $symbolPath
    }
    $pdb = Resolve-ExistingPath $pdb 'PDB'

    # /PDBALTPATH and the explicit .sympath make the dump independent of the build machine path.
    & $debuggerPath -z $dumpPath -y (Split-Path -Parent $pdb) -c '.lines; .ecxr; kP; q'
    exit $LASTEXITCODE
}

$debugger = Get-Command gdb -ErrorAction SilentlyContinue
if ($null -eq $debugger) {
    throw 'gdb was not found. Install gdb to symbolize a Linux core.'
}

$debugFile = if ((Get-Item -LiteralPath $symbolPath).PSIsContainer) {
    Join-Path $symbolPath (([IO.Path]::GetFileName($binaryPath)) + '.debug')
} else {
    $symbolPath
}
$debugFile = Resolve-ExistingPath $debugFile 'DWARF debug file'

& $debugger.Source --batch `
    -ex 'set pagination off' `
    -ex "set debug-file-directory `"$(Split-Path -Parent $debugFile)`"" `
    -ex "symbol-file `"$debugFile`"" `
    -ex 'thread apply all bt full' `
    -ex 'info sharedlibrary' `
    $binaryPath $dumpPath
exit $LASTEXITCODE
