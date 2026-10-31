# KAST standalone firmware flasher (end-user path).
#
# This script is bundled inside KAST-Setup.exe. It flashes the prebuilt
# firmware images that ship next to it (firmware\) onto a KAST box over USB,
# using the bundled esptool (flasher\esptool.exe). The user never needs
# ESP-IDF, Python, or a terminal.
#
# Development keeps using canonical idf.py; this file is ONLY the packaged
# end-user path (approved release direction 2026-10-31).
#
# Usage (also called by companion\first_run.ps1):
#   kast_flash.ps1 -Action list
#   kast_flash.ps1 -Action detect  [-Port COM9]
#   kast_flash.ps1 -Action flash   [-Port COM9]
#   kast_flash.ps1 -Action verify  [-Port COM9] [-TimeoutSec 20]
#   kast_flash.ps1 -Action erase   [-Port COM9]        # Advanced Recovery only
#
# Output: one log line per step on stdout. Exit code 0 = success, 1 = failure.
# -Action list prints "PORT<TAB>COMx<TAB>friendly name" lines for the wizard.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('list', 'detect', 'flash', 'verify', 'erase', 'driver')]
    [string]$Action,
    [string]$Port = '',
    [int]$TimeoutSec = 20
)

$ErrorActionPreference = 'Stop'
$root       = Split-Path -Parent $PSScriptRoot          # install root
$flasherDir = if (Test-Path (Join-Path $PSScriptRoot 'esptool.exe')) { $PSScriptRoot } else { Join-Path $root 'flasher' }
$firmwareDir = Join-Path $root 'firmware'
$esptool    = Join-Path $flasherDir 'esptool.exe'

function Fail([string]$msg) { Write-Host "ERROR: $msg"; exit 1 }

function Test-EsptoolPresent { if (-not (Test-Path $esptool)) { Fail "bundled esptool not found at $esptool" } }

# --- port discovery ---------------------------------------------------------
# The box is an ESP32-S3 whose native USB is the console; Windows 10/11 uses
# the built-in usbser driver, so it shows up as a plain USB serial COM port.
function Get-KastPorts {
    $ports = @()
    try {
        $devs = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
            Where-Object { $_.Name -match '\(COM\d+\)$' }
        foreach ($d in $devs) {
            if ($d.Name -match '\((COM\d+)\)$') {
                $ports += [pscustomobject]@{ Port = $Matches[1]; Name = ($d.Name -replace '\s*\(COM\d+\)$', '') }
            }
        }
    } catch {
        foreach ($p in [System.IO.Ports.SerialPort]::GetPortNames()) {
            $ports += [pscustomobject]@{ Port = $p; Name = 'Serial port' }
        }
    }
    $ports | Sort-Object { [int]($_.Port -replace '\D', '') }
}

function Resolve-KastPort {
    if ($Port) { return $Port }
    $candidates = @(Get-KastPorts)
    if ($candidates.Count -eq 0) { Fail 'no COM ports found - plug in the KAST box with a USB data cable' }
    if ($candidates.Count -eq 1) { return $candidates[0].Port }
    # Several ports: the caller (wizard) must pass -Port after user picks one.
    Fail "multiple COM ports found ($($candidates.Port -join ', ')) - rerun with -Port"
}

function Invoke-Esptool([string[]]$ToolArgs) {
    # Capture combined output; esptool writes progress AND warnings to stderr.
    # Windows PowerShell turns any stderr line into a NativeCommandError under
    # ErrorActionPreference=Stop, so drop to Continue for the native call.
    Test-EsptoolPresent
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $esptool @ToolArgs 2>&1 | Out-String
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    return @{ Text = $out; Code = $code }
}

switch ($Action) {

    'list' {
        foreach ($p in Get-KastPorts) { Write-Output ("PORT`t{0}`t{1}" -f $p.Port, $p.Name) }
        exit 0
    }

    'detect' {
        $p = Resolve-KastPort
        Write-Host "Probing $p for an ESP32-S3..."
        $r = Invoke-Esptool @('--chip', 'esp32s3', '--port', $p, 'chip-id')
        if ($r.Code -ne 0 -or $r.Text -notmatch 'ESP32-S3') {
            Write-Host $r.Text
            Fail "no ESP32-S3 answered on $p - check the cable and that no other app holds the port"
        }
        $mac = ([regex]'MAC:\s*([0-9a-f:]+)').Match($r.Text).Groups[1].Value
        Write-Host "OK: ESP32-S3 found on $p (MAC $mac)"
        exit 0
    }

    'flash' {
        $p = Resolve-KastPort
        $boot = Join-Path $firmwareDir 'bootloader.bin'
        $part = Join-Path $firmwareDir 'partition-table.bin'
        $app  = Join-Path $firmwareDir 'KAST.bin'
        foreach ($f in @($boot, $part, $app)) {
            if (-not (Test-Path $f)) { Fail "firmware image missing: $f" }
        }
        Write-Host "Flashing KAST firmware to $p (this takes about a minute)..."
        $r = Invoke-Esptool @(
            '--chip', 'esp32s3', '--port', $p, '--baud', '460800',
            '--before', 'default-reset', '--after', 'hard-reset',
            'write-flash',
            '--flash-mode', 'dio', '--flash-freq', '80m', '--flash-size', '16MB',
            '0x0', $boot, '0x8000', $part, '0x10000', $app
        )
        Write-Host $r.Text
        if ($r.Code -ne 0) { Fail 'flash failed - try a different USB cable/port, then rerun' }
        Write-Host 'OK: firmware written.'
        exit 0
    }

    'verify' {
        # After flashing, esptool hard-resets the box. Open the console and
        # look for the firmware's BUILD: banner, which proves the expected
        # image actually booted (Commandment VI freshness check, end-user edition).
        #
        # This uses raw Win32 serial via Add-Type, NOT System.IO.Ports:
        # the CH340 driver on this board truncates .NET SerialPort reads
        # mid-boot (bench-proven 2026-10-31: pyserial 100% OK, SerialPort 0/5).
        # Reset line truth table: RTS = EN, DTR = IO0 (both active-asserted).
        $p = Resolve-KastPort
        Write-Host "Watching $p for the KAST boot banner (up to $TimeoutSec seconds)..."
        if (-not ('KastSerial' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class KastSerial {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr CreateFile(string n, uint acc, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(IntPtr h, byte[] buf, uint count, out uint done, IntPtr ov);
    [DllImport("kernel32.dll")] static extern bool EscapeCommFunction(IntPtr h, uint code);
    [DllImport("kernel32.dll")] static extern bool GetCommState(IntPtr h, ref DCB dcb);
    [DllImport("kernel32.dll")] static extern bool SetCommState(IntPtr h, ref DCB dcb);
    [DllImport("kernel32.dll")] static extern bool SetCommTimeouts(IntPtr h, ref TO t);
    [DllImport("kernel32.dll")] static extern bool PurgeComm(IntPtr h, uint flags);

    [StructLayout(LayoutKind.Sequential)]
    struct DCB {
        public uint Length, BaudRate, Flags;
        public ushort wReserved, XonLim, XoffLim;
        public byte ByteSize, Parity, StopBits;
        public sbyte XonChar, XoffChar, ErrorChar, EofChar, EvtChar;
        public ushort wReserved1;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct TO { public uint a, b, c, d, e; }

    const uint GENERIC_RW = 0xC0000000, OPEN_EXISTING = 3;
    const uint SETRTS = 3, CLRRTS = 4, CLRDTR = 6, PURGE_RXCLEAR = 8;

    // Returns the captured boot text (empty on open failure).
    public static string WatchBoot(string port, int timeoutSec) {
        IntPtr h = CreateFile("\\\\.\\" + port, GENERIC_RW, 0, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (h == (IntPtr)(-1)) return "";
        try {
            DCB dcb = new DCB();
            dcb.Length = (uint)Marshal.SizeOf(typeof(DCB));
            GetCommState(h, ref dcb);
            dcb.BaudRate = 115200; dcb.ByteSize = 8; dcb.Parity = 0; dcb.StopBits = 0;
            dcb.Flags = (dcb.Flags | 1u) & ~0x30u & ~0x3000u; // fBinary=1; manual DTR/RTS
            SetCommState(h, ref dcb);
            TO t = new TO { a = 0xFFFFFFFF }; // ReadFile returns with whatever is available
            SetCommTimeouts(h, ref t);
            EscapeCommFunction(h, CLRDTR);   // stay out of download mode
            EscapeCommFunction(h, CLRRTS);   // out of reset
            Thread.Sleep(300);
            PurgeComm(h, PURGE_RXCLEAR);
            EscapeCommFunction(h, SETRTS);   // pulse EN: reset the box
            Thread.Sleep(150);
            EscapeCommFunction(h, CLRRTS);
            var sb = new StringBuilder();
            var buf = new byte[8192];
            var deadline = DateTime.UtcNow.AddSeconds(timeoutSec);
            while (DateTime.UtcNow < deadline) {
                uint got;
                if (ReadFile(h, buf, (uint)buf.Length, out got, IntPtr.Zero) && got > 0)
                    sb.Append(Encoding.ASCII.GetString(buf, 0, (int)got));
                if (sb.ToString().Contains("BUILD:")) break;
                Thread.Sleep(50);
            }
            return sb.ToString();
        } finally {
            CloseHandle(h);
        }
    }
}
'@
        }
        $boot = [KastSerial]::WatchBoot($p, $TimeoutSec)
        if (-not $boot) { Fail "could not open $p - is another app holding it?" }
        $m = ([regex]'BUILD:\s*(.+)').Match($boot)
        if (-not $m.Success) { Fail 'no KAST boot banner seen - flash may have failed or the cable is charge-only' }
        Write-Host ("OK: box booted firmware " + $m.Groups[1].Value.Trim())
        exit 0
    }

    'erase' {
        # ADVANCED RECOVERY ONLY. Caller must have obtained explicit user
        # confirmation before invoking this (approved direction).
        $p = Resolve-KastPort
        Write-Host "Erasing the entire flash on $p..."
        $r = Invoke-Esptool @('--chip', 'esp32s3', '--port', $p, 'erase-flash')
        Write-Host $r.Text
        if ($r.Code -ne 0) { Fail 'erase failed' }
        Write-Host 'OK: flash erased. Run a full flash before using the box again.'
        exit 0
    }

    'driver' {
        # Install the bundled, Microsoft-WHCP-signed CH340 USB-serial driver
        # (exported from a working KAST machine's driver store; wch.cn
        # explicitly permits redistribution with products). Needs elevation;
        # the wizard relaunches this action with -Verb RunAs.
        $inf = Join-Path $root 'driver\ch341ser.inf'
        if (-not (Test-Path $inf)) { Fail "driver package missing: $inf" }
        Write-Host 'Installing the CH340 USB-serial driver...'
        $out = & pnputil.exe /add-driver $inf /install 2>&1 | Out-String
        Write-Host $out
        if ($LASTEXITCODE -ne 0) { Fail 'driver install failed' }
        Write-Host 'OK: driver installed. Unplug and replug the box, then Detect again.'
        exit 0
    }
}
