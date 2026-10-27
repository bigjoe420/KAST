# KAST Companion — tray host for the PC relay (D-15).
#
# Double-clickable: keeps the relay running with no terminal window,
# advertises it with a UDP discovery beacon (device auto-finds the PC —
# no IP typing), and surfaces device connections as balloon tips.
#
# Layout: this script lives in companion\ next to kast.ico; the relay is
# ..\relay\server.js. Node is taken from ..\..\node24\current (portable
# bench install) when present, else from PATH.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$companionDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$relayDir     = Resolve-Path (Join-Path $companionDir '..\relay')
$logFile      = Join-Path $relayDir 'tray.log'
$benchFlag    = Join-Path $companionDir 'bench-mode.txt'
$iconPath     = Join-Path $companionDir 'kast.ico'

$candidates = @(
    'C:\Development\node24\current\node.exe',
    (Join-Path $relayDir '..\node24\current\node.exe'),
    'node.exe'
)
$script:nodeExe = $candidates | Where-Object { $_ -eq 'node.exe' -or (Test-Path $_) } | Select-Object -First 1
if (-not $script:nodeExe) {
    [System.Windows.Forms.MessageBox]::Show(
        'Node.js was not found. Install it or restore the portable node24 folder.',
        'KAST Companion', 'OK', 'Error') | Out-Null
    exit 1
}

$script:relayProc = $null
$script:benchMode = Test-Path $benchFlag

function Stop-Relay {
    if ($script:relayProc -and -not $script:relayProc.HasExited) {
        Stop-Process -Id $script:relayProc.Id -Force -ErrorAction SilentlyContinue
    }
    $script:relayProc = $null
}

function Start-Relay {
    Stop-Relay
    # .NET Framework (Windows PowerShell 5.1) has no ArgumentList —
    # build the classic argument string instead
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:nodeExe
    $psi.Arguments = if ($script:benchMode) { 'server.js --simulate-chat' } else { 'server.js' }
    $psi.WorkingDirectory = $relayDir.Path
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $script:relayProc = [System.Diagnostics.Process]::Start($psi)
    $script:lastLogPos = 0
}

function Set-FirewallRules {
    # needs admin; failure is fine — the relay just won't be reachable
    # until the owner allows inbound TCP 8765 / UDP 5353 manually
    $ok = $false
    try {
        netsh advfirewall firewall add rule name="KAST Relay (TCP 8765)" dir=in action=allow protocol=TCP localport=8765 | Out-Null
        $ok = $true
    } catch { }
    return $ok
}

# --- tray UI -----------------------------------------------------------
$icon = New-Object System.Windows.Forms.NotifyIcon
$icon.Icon = New-Object System.Drawing.Icon($iconPath)
$icon.Text = 'KAST Companion — relay stopped'
$icon.Visible = $true

$menu = New-Object System.Windows.Forms.ContextMenuStrip
$miStatus = $menu.Items.Add('starting...')
$miStatus.Enabled = $false
$menu.Items.Add('-') | Out-Null
$miRestart = $menu.Items.Add('Restart relay')
$miBench = $menu.Items.Add('Bench mode (simulated chat)')
$miBench.Checked = $script:benchMode
$miLogs = $menu.Items.Add('Open relay folder')
$menu.Items.Add('-') | Out-Null
$miQuit = $menu.Items.Add('Quit')
$icon.ContextMenuStrip = $menu

$balloon = {
    param($title, $text, $kind)
    $icon.BalloonTipTitle = $title
    $icon.BalloonTipText = $text
    $icon.BalloonTipIcon = $kind
    $icon.ShowBalloonTip(4000)
}

$miRestart.Add_Click({ Start-Relay })
$miBench.Add_Click({
    $script:benchMode = -not $script:benchMode
    $miBench.Checked = $script:benchMode
    if ($script:benchMode) { New-Item -ItemType File -Path $benchFlag -Force | Out-Null }
    else { Remove-Item $benchFlag -Force -ErrorAction SilentlyContinue }
    Start-Relay
})
$miLogs.Add_Click({ Start-Process explorer.exe $relayDir.Path })
$miQuit.Add_Click({
    Stop-Relay
    $icon.Visible = $false
    [System.Windows.Forms.Application]::Exit()
})

# --- first start ---------------------------------------------------------
$fwOk = Set-FirewallRules
Start-Relay
if (-not $fwOk) {
    & $balloon 'KAST Companion' 'Firewall rules not set (needs admin once) — if the device cannot connect, allow inbound TCP 8765.' 'Warning'
}

# --- monitor loop: relay liveness + log-driven events --------------------
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1500
$script:lastLogPos = 0
$script:wasAlive = $false
$script:seenConnect = $false

$timer.Add_Tick({
    # never let a tick kill the tray — child streams throw after exit
    try {
        # relay alive?
        $alive = $script:relayProc -and -not $script:relayProc.HasExited
        if (-not $alive) {
            if ($script:wasAlive) { & $balloon 'KAST relay' 'Relay stopped unexpectedly — restarting it.' 'Warning' }
            Start-Relay
            $alive = ($script:relayProc -and -not $script:relayProc.HasExited)
        }
        $script:wasAlive = $alive
        $state = if ($script:benchMode) { 'relay up (bench mode)' } else { 'relay up' }
        $miStatus.Text = if ($alive) { $state } else { 'relay DOWN — retrying' }
        $icon.Text = "KAST Companion — $(if ($alive) { 'relay running' } else { 'relay DOWN' })"

        # drain relay output so the pipe never backpressures the child
        if ($alive -and $script:relayProc) {
            foreach ($streamName in 'StandardOutput', 'StandardError') {
                try {
                    $stream = $script:relayProc.$streamName
                    while ($stream.Peek() -ge 0) {
                        $line = $stream.ReadLine()
                        if ($line) { Add-Content -Path $logFile -Value $line -Encoding UTF8 }
                    }
                } catch { }
            }
            if ((Test-Path $logFile)) {
                $len = (Get-Item $logFile).Length
                if ($len -lt $script:lastLogPos) { $script:lastLogPos = 0 }
                if ($len -gt $script:lastLogPos) {
                    $sr = $null
                    try {
                        $sr = New-Object System.IO.StreamReader($logFile)
                        $sr.BaseStream.Seek($script:lastLogPos, 'Begin') | Out-Null
                        $sr.DiscardBufferedData()
                        while (-not $sr.EndOfStream) {
                            $l = $sr.ReadLine()
                            if ($l -match 'device connected') {
                                & $balloon 'KAST relay' 'Device connected.' 'Info'
                                $script:seenConnect = $true
                            } elseif ($l -match 'device disconnected') {
                                & $balloon 'KAST relay' 'Device disconnected.' 'Info'
                            }
                        }
                        $script:lastLogPos = $sr.BaseStream.Position
                    } catch { } finally { if ($sr) { $sr.Close() } }
                }
            }
        }
    } catch { }
})
$timer.Start()

[System.Windows.Forms.Application]::Run()
