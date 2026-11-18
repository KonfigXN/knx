#Requires -Version 5.1

[CmdletBinding()]
param(
    [ValidateSet('stable', 'debug', 'builds', 'kernel', 'kernel-bin', 'iso', 'clean')]
    [string]$Command = 'stable',

    [ValidateSet('stable', 'debug')]
    [string]$Profile = 'stable',

    [ValidateRange(1, 64)]
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
if (-not $repoRoot) {
    $repoRoot = (Get-Location).Path
}

function Get-ToolPath {
    param(
        [string]$EnvironmentVariable,
        [string]$Default,
        [string]$Description
    )

    $value = [Environment]::GetEnvironmentVariable($EnvironmentVariable)
    if (-not $value) {
        $value = $Default
    }
    if (-not (Test-Path -LiteralPath $value)) {
        throw "$Description not found at '$value'. Set $EnvironmentVariable to override."
    }
    return $value
}

$msysRoot = Get-ToolPath 'KNX_MSYS2_ROOT' `
    'C:\Users\Konfig\knx-work\msys64' 'MSYS2 root'
$llvmBin = Get-ToolPath 'KNX_LLVM_BIN' `
    'C:\Program Files\LLVM\bin' 'LLVM bin directory'
$rustBin = Get-ToolPath 'KNX_RUST_BIN' `
    'C:\Program Files\Rust stable MSVC 1.99\bin' 'Rust bin directory'
$rustSysroot = Get-ToolPath 'KNX_RUST_SYSROOT' `
    'C:\Users\Konfig\knx-work\rust-std-extract\rust-std-1.99.1-x86_64-unknown-none\rust-std-x86_64-unknown-none' `
    'Rust x86_64-unknown-none sysroot'

$bash = Join-Path $msysRoot 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) {
    throw "MSYS2 bash not found at '$bash'. Set KNX_MSYS2_ROOT to override."
}

$env:PATH = @(
    $llvmBin,
    $rustBin,
    (Join-Path $msysRoot 'mingw64\bin'),
    (Join-Path $msysRoot 'usr\bin'),
    $env:PATH
) -join ';'
$env:KNX_ROOT = $repoRoot
$env:KNX_RUST_SYSROOT = $rustSysroot

$buildDir = "build/$Profile"
switch ($Command) {
    'stable' { $makeArgs = @('stable') }
    'debug' { $makeArgs = @('debug') }
    'builds' { $makeArgs = @('builds') }
    'kernel' { $makeArgs = @('kernel') }
    'kernel-bin' { $makeArgs = @('kernel-bin') }
    'iso' { $makeArgs = @("BUILD=$buildDir", "PROFILE=$Profile", 'iso') }
    'clean' { $makeArgs = @('clean') }
}

$makeLine = ($makeArgs | ForEach-Object { "'" + $_.Replace("'", "'\''") + "'" }) -join ' '
$script = 'cd "$(cygpath "$KNX_ROOT")/KNX" || exit 1; make ' + $makeLine + ' -j' + $Jobs

Write-Host "KNX kernel $Command ($Profile), jobs=$Jobs" -ForegroundColor Cyan

$scriptFile = Join-Path $env:TEMP ("knx-build-{0}.sh" -f $PID)
[System.IO.File]::WriteAllText(
    $scriptFile,
    ($script -replace "`r`n", "`n") + "`n",
    [System.Text.UTF8Encoding]::new($false)
)
try {
    & $bash ($scriptFile -replace '\\', '/')
    $status = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $scriptFile -ErrorAction SilentlyContinue
}
exit $status
