param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot "fetch-deps.ps1")
& (Join-Path $PSScriptRoot "make-icon.ps1")

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Installer (vswhere.exe) not found."
}
$msbuild = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) {
    throw "MSBuild not found. Install the 'Desktop development with C++' workload."
}

& $msbuild (Join-Path $root "WindowsClean.sln") /p:Configuration=$Config /p:Platform=x64 /m /v:m
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host ""
Write-Host "Build OK. Output:"
Get-ChildItem (Join-Path $root "bin\x64\$Config\WindowsClean*") | Select-Object Name, Length

