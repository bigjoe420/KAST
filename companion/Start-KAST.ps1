# KAST one-click start — launches OBS and the KAST Companion tray.
# Safe to run any number of times: each step starts only what is missing.
# (Owner decision 2026-11-12: one visible button to start the stack;
#  deeper OBS integration waits for the OBS fork.)
[CmdletBinding()]
param()

$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms

# --- OBS: first existing candidate wins. When the OBS fork lands, put its
#     path at the TOP of this list (owner 2026-11-12). ---
$obsCandidates = @(
    'C:\Program Files\obs-studio\bin\64bit\obs64.exe',
    'C:\Program Files (x86)\obs-studio\bin\64bit\obs64.exe'
)
$obsExe = $obsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not (Get-Process obs64)) {
    if ($obsExe) {
        Start-Process $obsExe
    } else {
        [System.Windows.Forms.MessageBox]::Show(
            'OBS was not found in the standard install paths.',
            'KAST', 'OK', 'Warning') | Out-Null
    }
}

# --- KAST Companion tray (relay + discovery beacon + tray icon) ---
# Port 8765 listening = a relay is already up; tray.ps1 also has its own
# single-instance mutex, so this check is belt-and-braces only.
$portUp = Get-NetTCPConnection -LocalPort 8765 -State Listen
if (-not $portUp) {
    $tray = Join-Path $PSScriptRoot 'tray.ps1'
    Start-Process powershell.exe -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-WindowStyle', 'Hidden', '-File', "`"$tray`""
    ) | Out-Null
}
