param(
    [string]$EverythingVersion = "1.5.0.1422b"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root "third_party"

New-Item -ItemType Directory -Force -Path $tp | Out-Null

$sdkZip = Join-Path $tp "Everything-SDK.zip"
$evZip  = Join-Path $tp "Everything-$EverythingVersion.x64.zip"

if (-not (Test-Path $sdkZip)) {
    Write-Host "Downloading Everything SDK..."
    & curl.exe -L --connect-timeout 15 --max-time 300 -o $sdkZip "https://www.voidtools.com/Everything-SDK.zip"
    if ($LASTEXITCODE -ne 0) { throw "Failed to download Everything SDK" }
}
if (-not (Test-Path $evZip)) {
    Write-Host "Downloading Everything $EverythingVersion portable..."
    & curl.exe -L --connect-timeout 15 --max-time 300 -o $evZip "https://www.voidtools.com/Everything-$EverythingVersion.x64.zip"
    if ($LASTEXITCODE -ne 0) { throw "Failed to download Everything portable" }
}

$sdkDir = Join-Path $tp "Everything-SDK"
$evDir  = Join-Path $tp "Everything-portable"

if (-not (Test-Path (Join-Path $sdkDir "include\Everything.h")) -or
    -not (Test-Path (Join-Path $sdkDir "dll\Everything64.dll"))) {
    Write-Host "Extracting Everything SDK..."
    Expand-Archive -LiteralPath $sdkZip -DestinationPath $sdkDir -Force
}
if (-not (Test-Path (Join-Path $evDir "Everything.exe"))) {
    Write-Host "Extracting Everything portable..."
    Expand-Archive -LiteralPath $evZip -DestinationPath $evDir -Force
}

Write-Host "Dependencies ready."
