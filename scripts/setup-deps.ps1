$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$downloadDir = Join-Path $root 'third_party\downloads'
$ffmpegRoot = Join-Path $root 'third_party\ffmpeg'
$zipName = 'ffmpeg-9.0.2_x86-windows-shared-lgpl.zip'
$zipPath = Join-Path $downloadDir $zipName
$url = 'https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/ffmpeg-9.0.2/ffmpeg-9.0.2_x86-windows-shared-lgpl.zip'
$expectedHash = '893BDBDFB334D02246098751A8FA06F1AA6903F7FAB504701748E1F8DF59026C'
$marker = Join-Path $ffmpegRoot 'bin\avformat-63.dll'

function Get-PeMachine([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    $br = New-Object IO.BinaryReader($fs)
    try {
        $fs.Position = 0x3C
        $pe = $br.ReadInt32()
        $fs.Position = $pe + 4
        return $br.ReadUInt16()
    } finally {
        $br.Close()
        $fs.Close()
    }
}

New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null
$header = Join-Path $root 'third_party\winamp-sdk\in2.h'
if (-not (Test-Path $header)) {
    throw "Missing authentic Winamp header: $header"
}

$needsExtract = -not (Test-Path $marker)
if (-not $needsExtract) {
    $machine = Get-PeMachine $marker
    if ($machine -ne 0x14C) {
        throw "Existing $marker is not x86 (machine 0x$($machine.ToString('X4')))."
    }
    Write-Host "FFmpeg x86 libraries already present."
    exit 0
}

if (-not (Test-Path $zipPath)) {
    Write-Host "Downloading $url"
    curl.exe -fL --retry 3 -o $zipPath $url
    if ($LASTEXITCODE -ne 0) { throw "Download failed with exit $LASTEXITCODE" }
}

$hash = (Get-FileHash -Algorithm SHA256 $zipPath).Hash
if ($hash -ne $expectedHash) {
    throw "SHA256 mismatch for $zipName. Expected $expectedHash, got $hash"
}

if (Test-Path $ffmpegRoot) {
    Remove-Item -Recurse -Force $ffmpegRoot
}
New-Item -ItemType Directory -Force -Path $ffmpegRoot | Out-Null
tar.exe -xf $zipPath -C $ffmpegRoot
if ($LASTEXITCODE -ne 0) { throw "Extract failed with exit $LASTEXITCODE" }
if (-not (Test-Path $marker)) { throw "Archive did not contain bin\avformat-63.dll" }
$machine = Get-PeMachine $marker
if ($machine -ne 0x14C) { throw "Downloaded FFmpeg is not x86." }
Write-Host "FFmpeg 9.0.2 x86 LGPL shared libraries are in third_party\ffmpeg"
