$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

& (Join-Path $PSScriptRoot 'setup-deps.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

function Find-VcVars {
    $candidates = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $installed = & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installed) { $candidates += $installed }
    }
    $candidates += @(
        'C:\VSBuildTools',
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools'
    )
    foreach ($rootPath in $candidates) {
        if (-not $rootPath) { continue }
        $vcvars = Join-Path $rootPath 'VC\Auxiliary\Build\vcvarsall.bat'
        if (Test-Path $vcvars) { return $vcvars }
    }
    return $null
}

$vcvars = Find-VcVars
if (-not $vcvars) {
    throw @"
The MSVC v143 toolset is not installed, so in_hls.dll cannot be compiled.
Install the Desktop development with C++ workload, or run an elevated command:
  C:\VSBuildTools\... is created by:
  vs_buildtools.exe install --installPath C:\VSBuildTools --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --passive --norestart --wait
"@
}

$sln = Join-Path $root 'winamp-hls.sln'
Write-Host "Using $vcvars"
cmd.exe /c "call `"$vcvars`" x86 && msbuild `"$sln`" /m /v:minimal /p:Configuration=Release /p:Platform=Win32"
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }

$out = Join-Path $root 'build\Win32\Release'
$test = Join-Path $out 'in_hls_tests.exe'
if (-not (Test-Path (Join-Path $out 'in_hls.dll'))) { throw "in_hls.dll was not produced" }
Write-Host "Running unit tests"
& $test
if ($LASTEXITCODE -ne 0) { throw "Unit tests failed with exit code $LASTEXITCODE" }

& (Join-Path $PSScriptRoot 'verify-arch.ps1') -Path $out
& (Join-Path $PSScriptRoot 'verify-arch.ps1') -Path (Join-Path $root 'third_party\ffmpeg\bin')
Write-Host "Build succeeded: $out\in_hls.dll"
