$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$version = '1.1.0'
$dist = Join-Path $root 'dist\in_hls-win32'
$nsi = Join-Path $root 'packaging\in_hls.nsi'
$nsisVersion = '3.13'
$nsisZipName = "nsis-$nsisVersion.zip"
$nsisZip = Join-Path $root "third_party\downloads\$nsisZipName"
$nsisUrl = "https://sourceforge.net/projects/nsis/files/NSIS%203/$nsisVersion/$nsisZipName/download"
$expectedHash = 'BA63DFFC4410EE89193E1CB5A41989991BD77C61068DA17E3156D136B7B0B3D8'
$toolsNsis = Join-Path $root "tools\nsis\nsis-$nsisVersion"
$bundledMakensis = Join-Path $toolsNsis 'makensis.exe'

function Find-Makensis {
    $candidates = @(
        $bundledMakensis,
        (Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'),
        (Join-Path $env:ProgramFiles 'NSIS\makensis.exe')
    )
    foreach ($path in $candidates) {
        if ($path -and (Test-Path $path)) { return $path }
    }
    $fromPath = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }
    return $null
}

function Install-BundledNsis {
    $downloadDir = Split-Path $nsisZip
    New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null

    if (-not (Test-Path $nsisZip)) {
        Write-Host "Downloading NSIS $nsisVersion"
        curl.exe -fL --retry 3 -o $nsisZip $nsisUrl
        if ($LASTEXITCODE -ne 0) { throw "Could not download NSIS from $nsisUrl" }
    }

    $hash = (Get-FileHash -Algorithm SHA256 $nsisZip).Hash
    if ($hash -ne $expectedHash) {
        throw "SHA256 mismatch for $nsisZipName. Expected $expectedHash, got $hash"
    }

    $extractRoot = Join-Path $root 'tools\nsis'
    if (Test-Path $extractRoot) {
        Remove-Item -Recurse -Force $extractRoot
    }
    New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
    Write-Host "Extracting NSIS $nsisVersion into $extractRoot"
    tar.exe -xf $nsisZip -C $extractRoot
    if ($LASTEXITCODE -ne 0) { throw "Could not extract $nsisZip" }
    if (-not (Test-Path $bundledMakensis)) {
        throw "Archive did not contain makensis.exe at $bundledMakensis"
    }
    return $bundledMakensis
}

if (-not (Test-Path (Join-Path $dist 'in_hls.dll'))) {
    Write-Host 'Package folder missing; running package.ps1'
    & (Join-Path $PSScriptRoot 'package.ps1')
}

$required = @(
    'in_hls.dll',
    'in_hls\avutil-61.dll',
    'in_hls\swresample-7.dll',
    'in_hls\swscale-10.dll',
    'in_hls\avcodec-63.dll',
    'in_hls\avformat-63.dll',
    'README.txt',
    'LICENSE.md',
    'THIRD-PARTY-NOTICES.md',
    'FFMPEG-LGPL-3.0.txt',
    'GPL-3.0.txt'
)
foreach ($name in $required) {
    $path = Join-Path $dist $name
    if (-not (Test-Path $path)) { throw "Package is incomplete: missing $path" }
}

if (-not (Test-Path $nsi)) { throw "Missing NSIS script: $nsi" }

$compiler = Find-Makensis
if (-not $compiler) {
    $compiler = Install-BundledNsis
}

Write-Host "Compiling installer with $compiler"
$previous = Get-Location
try {
    Set-Location (Join-Path $root 'packaging')
    & $compiler /V2 $nsi
    if ($LASTEXITCODE -ne 0) { throw "makensis failed with exit $LASTEXITCODE" }
} finally {
    Set-Location $previous
}

$setup = Join-Path $root "dist\in_hls-$version-win32-setup.exe"
if (-not (Test-Path $setup)) { throw "Installer was not written to $setup" }

Write-Host "Installer written to $setup"
Get-Item $setup | ForEach-Object { '{0,12} {1}' -f $_.Length, $_.FullName }
