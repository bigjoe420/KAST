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

function Update-RelayEnv {
    param([hashtable]$Values)
    $envFile = Join-Path $relayDir '.env'
    $lines = if (Test-Path $envFile) { @(Get-Content $envFile) } else { @() }
    foreach ($key in $Values.Keys) {
        $replacement = "$key=$($Values[$key])"
        $found = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match "^\s*$key\s*=") { $lines[$i] = $replacement; $found = $true }
        }
        if (-not $found) { $lines += $replacement }
    }
    Set-Content -Path $envFile -Value $lines -Encoding UTF8
}

function Invoke-ObsCommand {
    param([string]$Action, [string]$Value = '')
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $script:nodeExe
    $psi.Arguments = if ($Value -eq '') { "obs_command.js $Action" } else { "obs_command.js $Action $Value" }
    $psi.WorkingDirectory = $relayDir.Path
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { throw $stderr.Trim() }
    return $stdout.Trim()
}

function Show-ObsSetup {
    $running = Get-Process obs64 -ErrorAction SilentlyContinue
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'Set up KAST with OBS'
    $form.Size = New-Object System.Drawing.Size(540, 330)
    $form.StartPosition = 'CenterScreen'
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $label = New-Object System.Windows.Forms.Label
    $label.Location = New-Object System.Drawing.Point(18, 16)
    $label.Size = New-Object System.Drawing.Size(490, 105)
    $label.Text = "In OBS: Tools > WebSocket Server Settings.`r`nEnable the server, keep port 4455, then enter its password below.`r`nLeave the password blank only if OBS authentication is disabled.`r`n`r`nKAST adds only its own SFX scene and sources. Your scenes stay untouched."
    $form.Controls.Add($label)
    $pwLabel = New-Object System.Windows.Forms.Label
    $pwLabel.Text = 'OBS WebSocket password'
    $pwLabel.Location = New-Object System.Drawing.Point(18, 132)
    $pwLabel.Size = New-Object System.Drawing.Size(190, 24)
    $form.Controls.Add($pwLabel)
    $pw = New-Object System.Windows.Forms.TextBox
    $pw.Location = New-Object System.Drawing.Point(210, 128)
    $pw.Size = New-Object System.Drawing.Size(290, 24)
    $pw.UseSystemPasswordChar = $true
    $form.Controls.Add($pw)
    $ok = New-Object System.Windows.Forms.Button
    $ok.Text = 'Save and connect'
    $ok.Location = New-Object System.Drawing.Point(210, 180)
    $ok.Size = New-Object System.Drawing.Size(140, 34)
    $ok.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $form.AcceptButton = $ok
    $form.Controls.Add($ok)
    $cancel = New-Object System.Windows.Forms.Button
    $cancel.Text = 'Cancel'
    $cancel.Location = New-Object System.Drawing.Point(360, 180)
    $cancel.Size = New-Object System.Drawing.Size(90, 34)
    $cancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.CancelButton = $cancel
    $form.Controls.Add($cancel)
    if (-not $running) {
        $label.Text += "`r`n`r`nOBS is not running yet. You can save now, then open OBS."
        $form.Height = 370
    }
    if ($form.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return }
    Update-RelayEnv @{ KAST_OBS_ENABLED = '1'; KAST_OBS_URL = 'ws://127.0.0.1:4455'; KAST_OBS_PASSWORD = $pw.Text }
    Start-Relay
    if ($running) {
        Start-Sleep -Seconds 2
        try {
            Invoke-ObsCommand 'setup' | Out-Null
            & $balloon 'KAST OBS' 'Connected. KAST SFX is ready — use Test OBS sound to hear it.' 'Info'
        } catch {
            [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'KAST OBS setup needs attention', 'OK', 'Warning') | Out-Null
        }
    } else {
        & $balloon 'KAST OBS' 'Settings saved. OBS will connect automatically when it is running.' 'Info'
    }
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
$miObsStatus = $menu.Items.Add('OBS: not connected')
$miObsStatus.Enabled = $false
$miObsSetup = $menu.Items.Add('Setup OBS...')
$miObsTest = $menu.Items.Add('Test OBS sound')
$miSoundLibrary = $menu.Items.Add('Sound Library...')
$miObsVolume = $menu.Items.Add('SFX volume')
$miObsVol50 = $miObsVolume.DropDownItems.Add('50%')
$miObsVol100 = $miObsVolume.DropDownItems.Add('100%')
$miObsVol150 = $miObsVolume.DropDownItems.Add('150%')
$miObsRemove = $menu.Items.Add('Remove KAST SFX Setup')
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
$miObsSetup.Add_Click({ Show-ObsSetup })
$miSoundLibrary.Add_Click({
    $scriptPath = Join-Path $companionDir 'sound_library.ps1'
    Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -STA -File `"$scriptPath`""
})
$miObsTest.Add_Click({
    try { & $balloon 'KAST OBS' (Invoke-ObsCommand 'test') 'Info' }
    catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'KAST OBS test failed', 'OK', 'Warning') | Out-Null }
})
$miObsVol50.Add_Click({ try { Invoke-ObsCommand 'volume' '0.5' | Out-Null } catch { & $balloon 'KAST OBS' $_.Exception.Message 'Warning' } })
$miObsVol100.Add_Click({ try { Invoke-ObsCommand 'volume' '1' | Out-Null } catch { & $balloon 'KAST OBS' $_.Exception.Message 'Warning' } })
$miObsVol150.Add_Click({ try { Invoke-ObsCommand 'volume' '1.5' | Out-Null } catch { & $balloon 'KAST OBS' $_.Exception.Message 'Warning' } })
$miObsRemove.Add_Click({
    $answer = [System.Windows.Forms.MessageBox]::Show(
        'Remove only the KAST SFX scene and KAST SFX sources from OBS?',
        'Remove KAST SFX Setup', 'YesNo', 'Question')
    if ($answer -eq [System.Windows.Forms.DialogResult]::Yes) {
        try { & $balloon 'KAST OBS' (Invoke-ObsCommand 'remove') 'Info' }
        catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'KAST OBS cleanup failed', 'OK', 'Warning') | Out-Null }
    }
})
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
                            } elseif ($l -match '\[obs\] connected') {
                                $miObsStatus.Text = 'OBS: connected'
                            } elseif ($l -match '\[obs\] auth_failed') {
                                $miObsStatus.Text = 'OBS: wrong password — run Setup OBS'
                                & $balloon 'KAST OBS' 'OBS rejected the password. Choose Setup OBS to fix it.' 'Warning'
                            } elseif ($l -match '\[obs\] (disconnected|connecting|identifying)') {
                                $miObsStatus.Text = 'OBS: not connected'
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
