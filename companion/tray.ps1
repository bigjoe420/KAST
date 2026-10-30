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

# Single-instance guard: a double-click plus the startup shortcut can
# otherwise launch two trays, and the second relay child then spams
# EADDRINUSE while fighting the first for the port.
$script:instanceOwned = $false
$script:singleInstance = New-Object System.Threading.Mutex($true, 'KASTCompanionSingleInstance', [ref]$script:instanceOwned)
if (-not $script:instanceOwned) {
    [System.Windows.Forms.MessageBox]::Show(
        'KAST Companion is already running (check the system tray).',
        'KAST Companion', 'OK', 'Information') | Out-Null
    exit 0
}

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
    # No stdout/stderr redirection: server.js appends to tray.log itself.
    # Every PowerShell pipe-drain design failed (Peek blocks the UI thread
    # on an idle pipe; async DataReceived events crash the process — they
    # fire on threadpool threads with no PowerShell runspace). No pipe,
    # no drain, no deadlock, no crash class at all.
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
$miCopy = $menu.Items.Add('Copy device target (IP:port)')
$miAuth = $menu.Items.Add('Authorize with Kick...')
$miAuth.Visible = $false
$menu.Items.Add('-') | Out-Null
$miQuit = $menu.Items.Add('Quit')
$icon.ContextMenuStrip = $menu

# Relay prints the Kick authorize URL across two log lines when tokens
# are missing: a banner, then the URL itself. Capture it, expose it as a
# menu item, and hide it again once tokens land.
$script:kickAuthUrl = $null
$script:kickAuthPending = $false
$miAuth.Add_Click({
    if ($script:kickAuthUrl) { Start-Process $script:kickAuthUrl }
})

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
$miCopy.Add_Click({
    $ip = Get-LanIp
    if ($ip) { Set-Clipboard -Value "${ip}:8765" } else { Set-Clipboard -Value '' }
})
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
$script:lastIp = $null

# The address the device should aim at: the IPv4 of the adapter that owns
# the default route (the active Wi-Fi/hotspot), not loopback/virtuals.
function Get-LanIp {
    try {
        $route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
                 Sort-Object RouteMetric | Select-Object -First 1
        if ($route) {
            $addr = Get-NetIPAddress -AddressFamily IPv4 -InterfaceIndex $route.InterfaceIndex -ErrorAction SilentlyContinue |
                    Where-Object { $_.IPAddress -notlike '169.254*' -and $_.IPAddress -ne '127.0.0.1' } |
                    Select-Object -First 1
            if ($addr) { return $addr.IPAddress }
        }
    } catch { }
    return $null
}

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

        # surface the current device target IP — moving networks changes
        # it, and typing the wrong one is the #1 setup pain (D-15)
        $lanIp = Get-LanIp
        $target = if ($lanIp) { "${lanIp}:8765" } else { 'IP unknown' }
        $state = if ($script:benchMode) { 'relay up (bench mode)' } else { 'relay up' }
        $miStatus.Text = if ($alive) { "$state - device target $target" } else { "relay DOWN - device target $target" }
        $icon.Text = "KAST Companion - $target"
        if ($lanIp -and $script:lastIp -and $lanIp -ne $script:lastIp) {
            & $balloon 'KAST relay' "Network changed - device target is now $target. The box finds this PC by itself on most networks; type this only if it does not connect." 'Info'
        }
        if ($lanIp) { $script:lastIp = $lanIp }

        # relay output is drained by the async line events registered in
        # Start-Relay; here we only scan the log for state changes
        if ($alive -and $script:relayProc) {
            if ((Test-Path $logFile)) {
                $len = (Get-Item $logFile).Length
                if ($len -lt $script:lastLogPos) { $script:lastLogPos = 0 }
                if ($len -gt $script:lastLogPos) {
                    $sr = $null
                    try {
                        # ReadWrite share: the async log writer in Start-Relay
                        # holds the file for append while we scan — a plain
                        # StreamReader (FileShare.Read) would lock it out and
                        # make the writer throw
                        $fs = [System.IO.File]::Open($logFile, 'Open', 'Read', 'ReadWrite')
                        $sr = New-Object System.IO.StreamReader($fs)
                        $sr.BaseStream.Seek($script:lastLogPos, 'Begin') | Out-Null
                        $sr.DiscardBufferedData()
                        while (-not $sr.EndOfStream) {
                            $l = $sr.ReadLine()
                            if ($l -match 'device connected') {
                                & $balloon 'KAST relay' 'Device connected.' 'Info'
                                $script:seenConnect = $true
                            } elseif ($l -match 'device disconnected') {
                                & $balloon 'KAST relay' 'Device disconnected.' 'Info'
                            } elseif ($l -match 'authorize once by opening this URL') {
                                # next relay log line is the URL itself
                                $script:kickAuthPending = $true
                            } elseif ($script:kickAuthPending -and $l -match 'https://\S+') {
                                $script:kickAuthUrl = ($Matches[0])
                                $script:kickAuthPending = $false
                                $miAuth.Visible = $true
                                & $balloon 'KAST relay' 'Kick login needed: right-click the tray and choose Authorize with Kick.' 'Info'
                            } elseif ($l -match 'Kick authorized') {
                                $script:kickAuthUrl = $null
                                $miAuth.Visible = $false
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
