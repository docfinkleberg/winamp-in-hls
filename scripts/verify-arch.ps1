param(
    [string]$Path
)

$ErrorActionPreference = 'Stop'

function Get-PeMachine([string]$file) {
    $fs = [IO.File]::OpenRead($file)
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

if (-not $Path) {
    $Path = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Win32\Release'
}

$files = Get-ChildItem -Path $Path -Recurse -File | Where-Object { $_.Extension -in '.dll', '.exe' }
if (-not $files) {
    throw "No binaries found in $Path"
}

$bad = @()
foreach ($file in $files) {
    $machine = Get-PeMachine $file.FullName
    $label = if ($machine -eq 0x14C) { 'x86' } elseif ($machine -eq 0x8664) { 'x64' } else { '0x{0:X4}' -f $machine }
    Write-Host ("{0,-28} {1}" -f $file.Name, $label)
    if ($machine -ne 0x14C) { $bad += $file.FullName }
}
if ($bad.Count -gt 0) {
    throw "These binaries are not Win32/x86:`n$($bad -join "`n")"
}
