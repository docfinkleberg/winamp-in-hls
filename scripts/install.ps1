param(
    [string]$WinampPlugins,
    [string]$PackageDir,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $PackageDir) {
    $PackageDir = Join-Path $root 'dist\in_hls-win32'
}

$dll = Join-Path $PackageDir 'in_hls.dll'
if (-not (Test-Path $dll)) {
    throw "Package not found at $PackageDir. Run scripts\package.ps1 first."
}

function Get-WinampPluginsDir {
    $candidates = @()

    foreach ($hive in @('HKCU:', 'HKLM:')) {
        try {
            $props = Get-ItemProperty -Path "$hive\Software\Winamp" -ErrorAction Stop
            if ($props.PSObject.Properties['(default)'] -and $props.'(default)') {
                $candidates += Join-Path $props.'(default)' 'Plugins'
            }
        } catch {}
    }

    try {
        $uninstall = Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Winamp' -ErrorAction Stop
        if ($uninstall.InstallLocation) {
            $candidates += Join-Path $uninstall.InstallLocation 'Plugins'
        }
    } catch {}

    $candidates += Join-Path ${env:ProgramFiles(x86)} 'Winamp\Plugins'
    $candidates += Join-Path $env:ProgramFiles 'Winamp\Plugins'

    foreach ($path in $candidates) {
        if ($path -and (Test-Path $path)) { return $path }
    }
    return $null
}

if (-not $WinampPlugins) {
    $WinampPlugins = Get-WinampPluginsDir
}
if (-not $WinampPlugins) {
    throw 'Could not find a Winamp Plugins folder. Pass -WinampPlugins "C:\Path\To\Winamp\Plugins".'
}
if (-not (Test-Path $WinampPlugins)) {
    throw "Winamp Plugins folder does not exist: $WinampPlugins"
}

$rootFiles = @(
    'in_hls.dll',
    'README.txt',
    'LICENSE.md',
    'THIRD-PARTY-NOTICES.md',
    'FFMPEG-LGPL-3.0.txt',
    'GPL-3.0.txt'
)
$ffmpegFiles = @(
    'avutil-61.dll',
    'swresample-7.dll',
    'swscale-10.dll',
    'avcodec-63.dll',
    'avformat-63.dll'
)

Write-Host "Installing from $PackageDir"
Write-Host "Destination:     $WinampPlugins"
$ffmpegDest = Join-Path $WinampPlugins 'in_hls'
if (-not $WhatIf) {
    New-Item -ItemType Directory -Force -Path $ffmpegDest | Out-Null
    foreach ($name in $ffmpegFiles) {
        $stale = Join-Path $WinampPlugins $name
        if (Test-Path $stale) {
            Remove-Item -Force $stale
            Write-Host "Removed old $name from Plugins"
        }
    }
}
foreach ($name in $rootFiles) {
    $source = Join-Path $PackageDir $name
    if (-not (Test-Path $source)) { throw "Missing package file: $source" }
    $dest = Join-Path $WinampPlugins $name
    if ($WhatIf) {
        Write-Host "Would copy $name"
    } else {
        Copy-Item -Force $source $dest
        Write-Host "Copied $name"
    }
}
foreach ($name in $ffmpegFiles) {
    $source = Join-Path $PackageDir "in_hls\$name"
    if (-not (Test-Path $source)) { throw "Missing package file: $source" }
    $dest = Join-Path $ffmpegDest $name
    if ($WhatIf) {
        Write-Host "Would copy in_hls\$name"
    } else {
        Copy-Item -Force $source $dest
        Write-Host "Copied in_hls\$name"
    }
}

if (-not $WhatIf) {
    Write-Host 'Done. Restart Winamp so it can load in_hls.dll.'
}
