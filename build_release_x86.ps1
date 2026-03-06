Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$solutionPath = Join-Path $repoRoot "l4d2vr.sln"

if (-not (Test-Path -LiteralPath $solutionPath)) {
    Write-Error "Solution file not found: $solutionPath"
}

$msbuildCmd = Get-Command msbuild -ErrorAction SilentlyContinue
if (-not $msbuildCmd) {
    Write-Error "msbuild not found in PATH. Open a Visual Studio Developer PowerShell and retry."
}

Write-Host "Building fixed target: Release|x86 from l4d2vr.sln"

& $msbuildCmd.Source `
    $solutionPath `
    /t:Build `
    /p:Configuration=Release `
    /p:Platform=x86 `
    /m

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Build succeeded."
