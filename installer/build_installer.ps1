# Build KAST-Setup.exe: stage installer\payload\ from the live project,
# then compile installer\KAST-Setup.iss with Inno Setup 6.
#
# Inputs it pulls together (approved self-contained design, 2026-10-31):
#   companion\*.ps1 + cmd + ico          -> {app}\companion
#   relay runtime (no tests/logs/env)    -> {app}\relay
#   relay\starter_sounds (12 sounds)     -> {app}\relay\starter_sounds
#   portable Node (node.exe only)        -> {app}\node24\current
#   esptool.exe + kast_flash.ps1         -> {app}\flasher
#   build\ firmware images               -> {app}\firmware
#   signed CH340 driver package          -> {app}\driver
#
# Tool zips are cached in installer\.cache (gitignored); payload\ and dist\
# are gitignored too — only this script, the .iss, and kast_flash.ps1 commit.
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File installer\build_installer.ps1
[CmdletBinding()]
param(
    [string]$Version = '0.1.0',
    [switch]$SkipDownloads   # fail instead of fetching missing .cache tools
)
$ErrorActionPreference = 'Stop'
$installerDir = $PSScriptRoot
$root = Split-Path -Parent $installerDir
$cache = Join-Path $installerDir '.cache'
$payload = Join-Path $installerDir 'payload'
$dist = Join-Path $installerDir 'dist'

$NODE_VERSION = '24.22.2'
$ESPTOOL_VERSION = '5.6.0'
$ISCC = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'

function Need([string]$path, [string]$what) {
    if (-not (Test-Path $path)) { throw "Missing $what - expected at $path" }
}

function Fetch([string]$url, [string]$dest, [string]$what) {
    if (Test-Path $dest) { return }
    if ($SkipDownloads) { throw "Missing $what in cache and -SkipDownloads was given: $url" }
    Write-Host "Downloading $what..."
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
}

# --- 1. ensure tool cache ---------------------------------------------------
New-Item -ItemType Directory -Force $cache | Out-Null
$nodeZip = Join-Path $cache 'node.zip'
$espZip  = Join-Path $cache 'esptool.zip'
Fetch "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-win-x64.zip" $nodeZip 'portable Node.js'
Fetch "https://github.com/espressif/esptool/releases/download/v$ESPTOOL_VERSION/esptool-v$ESPTOOL_VERSION-windows-amd64.zip" $espZip 'esptool'

if (-not (Test-Path (Join-Path $cache 'node-extracted'))) {
    Write-Host 'Extracting Node...'
    Expand-Archive $nodeZip (Join-Path $cache 'node-extracted') -Force
}
if (-not (Test-Path (Join-Path $cache 'esp-extracted'))) {
    Write-Host 'Extracting esptool...'
    Expand-Archive $espZip (Join-Path $cache 'esp-extracted') -Force
}
$nodeExe = Get-ChildItem (Join-Path $cache 'node-extracted') -Recurse -Filter node.exe | Select-Object -First 1
$esptoolExe = Get-ChildItem (Join-Path $cache 'esp-extracted') -Recurse -Filter esptool.exe | Select-Object -First 1
Need $nodeExe.FullName 'node.exe in cache'
Need $esptoolExe.FullName 'esptool.exe in cache'

# The CH340 driver package is exported once from a working machine's driver
# store (Microsoft-WHCP signed; wch.cn permits redistribution with products).
$driverSrc = Join-Path $cache 'ch341ser'
Need (Join-Path $driverSrc 'ch341ser.inf') 'CH340 driver package - export it once with: pnputil /export-driver oemNNN.inf installer\.cache\ch341ser'

# --- 2. stage payload ---------------------------------------------------------
Write-Host 'Staging payload...'
if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $payload 'companion') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $payload 'relay\starter_sounds') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $payload 'relay\node_modules') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $payload 'node24\current') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $payload 'flasher') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $payload 'firmware') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $payload 'driver') | Out-Null

# Companion: every .ps1 + the launcher + icon. Never logs.
Copy-Item (Join-Path $root 'companion\*.ps1') (Join-Path $payload 'companion')
Copy-Item (Join-Path $root 'companion\KAST Companion.cmd') (Join-Path $payload 'companion')
Copy-Item (Join-Path $root 'companion\kast.ico') (Join-Path $payload 'companion')

# Relay runtime whitelist: no tests, logs, .env, sounds\, graphics\,
# scene_tiles.json — those are per-user and get created at runtime.
$relayFiles = @(
    'server.js', 'obs.js', 'obs_sfx.js', 'obs_gfx.js', 'obs_scenes.js',
    'obs_command.js', 'gfx_command.js', 'scene_command.js', 'sound_command.js',
    'sound_library.js', 'gfx_library.js', 'play_sfx.ps1', 'package.json'
)
foreach ($f in $relayFiles) {
    Need (Join-Path $root "relay\$f") "relay file $f"
    Copy-Item (Join-Path $root "relay\$f") (Join-Path $payload 'relay')
}
Copy-Item (Join-Path $root 'relay\starter_sounds\*.mp3') (Join-Path $payload 'relay\starter_sounds')
$starterCount = (Get-ChildItem (Join-Path $payload 'relay\starter_sounds') -Filter *.mp3).Count
if ($starterCount -ne 12) { throw "expected 12 starter sounds, found $starterCount" }
# ws is the only dependency and has no sub-dependencies.
Copy-Item (Join-Path $root 'relay\node_modules\ws') (Join-Path $payload 'relay\node_modules\ws') -Recurse

Copy-Item $nodeExe.FullName (Join-Path $payload 'node24\current\node.exe')
Copy-Item $esptoolExe.FullName (Join-Path $payload 'flasher\esptool.exe')
Copy-Item (Join-Path $installerDir 'kast_flash.ps1') (Join-Path $payload 'flasher')
Copy-Item (Join-Path $driverSrc '*') (Join-Path $payload 'driver')

# Firmware images: the last bench-accepted build. Offsets are fixed by
# flasher\kast_flash.ps1 (0x0 bootloader, 0x8000 partition table, 0x10000 app).
$firmwareMap = @{
    'build\bootloader\bootloader.bin'          = 'bootloader.bin'
    'build\partition_table\partition-table.bin' = 'partition-table.bin'
    'build\KAST.bin'                            = 'KAST.bin'
}
foreach ($src in $firmwareMap.Keys) {
    Need (Join-Path $root $src) "firmware image $src - build the firmware first"
    Copy-Item (Join-Path $root $src) (Join-Path $payload ('firmware\' + $firmwareMap[$src]))
}

# --- 3. compile -----------------------------------------------------------------
Need $ISCC 'Inno Setup 6 - install with: winget install JRSoftware.InnoSetup'
Write-Host 'Compiling KAST-Setup.exe...'
New-Item -ItemType Directory -Force $dist | Out-Null
& $ISCC "/DAppVersion=$Version" (Join-Path $installerDir 'KAST-Setup.iss')
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }
$exe = Join-Path $dist 'KAST-Setup.exe'
Need $exe 'compiled installer'
$size = '{0:N1} MB' -f ((Get-Item $exe).Length / 1MB)
Write-Host "DONE: $exe ($size)"
