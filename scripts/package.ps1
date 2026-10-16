$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$built = Join-Path $root 'build\Win32\Release'
$dll = Join-Path $built 'in_hls.dll'
if (-not (Test-Path $dll)) {
    throw "Build the plug-in first: scripts\build.ps1. Missing $dll"
}

$dist = Join-Path $root 'dist\in_hls-win32'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist | Out-Null

Copy-Item $dll $dist
$runtime = @(
    'avutil-61.dll',
    'swresample-7.dll',
    'swscale-10.dll',
    'avcodec-63.dll',
    'avformat-63.dll'
)
$bin = Join-Path $root 'third_party\ffmpeg\bin'
foreach ($name in $runtime) {
    $source = Join-Path $bin $name
    if (-not (Test-Path $source)) { throw "Missing FFmpeg runtime $source" }
    Copy-Item $source $dist
}

Copy-Item (Join-Path $root 'packaging\README.txt') (Join-Path $dist 'README.txt')
Copy-Item (Join-Path $root 'LICENSE.md') (Join-Path $dist 'LICENSE.md')
Copy-Item (Join-Path $root 'THIRD-PARTY-NOTICES.md') (Join-Path $dist 'THIRD-PARTY-NOTICES.md')
Copy-Item (Join-Path $root 'third_party\ffmpeg\LICENSE.txt') (Join-Path $dist 'FFMPEG-LGPL-3.0.txt')

$gpl = Join-Path $root 'third_party\licences\gpl-3.0.txt'
if (-not (Test-Path $gpl)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $gpl) | Out-Null
    curl.exe -fL --retry 3 -o $gpl 'https://www.gnu.org/licenses/gpl-3.0.txt'
    if ($LASTEXITCODE -ne 0) { throw "Could not download the GNU GPL-3.0 text required by the FFmpeg LGPLv3 build." }
}
Copy-Item $gpl (Join-Path $dist 'GPL-3.0.txt')

& (Join-Path $PSScriptRoot 'verify-arch.ps1') -Path $dist

$tests = Join-Path $built 'in_hls_tests.exe'
if (Test-Path $tests) {
    Write-Host "Loading the packaged FFmpeg DLLs from $dist"
    $env:IN_HLS_FFMPEG_DIR = $dist
    $env:IN_HLS_SKIP_HLS_MUX = '1'
    & $tests
    if ($LASTEXITCODE -ne 0) { throw "Packaged runtime failed the unit tests." }
    Remove-Item Env:IN_HLS_FFMPEG_DIR -ErrorAction SilentlyContinue
    Remove-Item Env:IN_HLS_SKIP_HLS_MUX -ErrorAction SilentlyContinue
}

Write-Host "Package written to $dist"
Get-ChildItem $dist | ForEach-Object { '{0,12} {1}' -f $_.Length, $_.Name }
