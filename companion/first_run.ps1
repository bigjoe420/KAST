# KAST first-run setup — launched once by KAST-Setup.exe (also re-runnable
# any time from the Start Menu). Walks the owner through the approved
# first-run flow: plug in box -> detect -> flash prebuilt firmware -> verify
# boot -> configure KAST-owned OBS items -> start Companion (tray).
# End users need only OBS, the box, and a USB cable — no ESP-IDF, no Node,
# no Python, no terminals.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$companionDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root         = Split-Path -Parent $companionDir
$flasher      = Join-Path $root 'flasher\kast_flash.ps1'
$iconPath     = Join-Path $companionDir 'kast.ico'

if (-not (Test-Path $flasher)) {
    [System.Windows.Forms.MessageBox]::Show("Flasher not found at $flasher. Reinstall KAST.", 'KAST setup', 'OK', 'Error') | Out-Null
    exit 1
}

# --- run a flasher action, stream its output into the log box --------------
$script:logBox = $null
function Write-Log([string]$line) {
    $script:logBox.AppendText($line + "`r`n")
    $script:logBox.SelectionStart = $script:logBox.TextLength
    $script:logBox.ScrollToCaret()
    [System.Windows.Forms.Application]::DoEvents()
}

function Invoke-Flasher([string]$action, [string]$port) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'powershell.exe'
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$flasher`"", '-Action', $action)
    if ($port) { $args += @('-Port', $port) }
    $psi.Arguments = $args -join ' '
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    while (-not $p.StandardOutput.EndOfStream) { Write-Log $p.StandardOutput.ReadLine() }
    $err = $p.StandardError.ReadToEnd().Trim()
    $p.WaitForExit()
    if ($err) { Write-Log $err }
    return ($p.ExitCode -eq 0)
}

# --- form -------------------------------------------------------------------
$form = New-Object System.Windows.Forms.Form
$form.Text = 'KAST first-time setup'
$form.Size = New-Object System.Drawing.Size(600, 640)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
if (Test-Path $iconPath) { $form.Icon = New-Object System.Drawing.Icon($iconPath) }

$steps = New-Object System.Windows.Forms.Label
$steps.Location = New-Object System.Drawing.Point(16, 12)
$steps.Size = New-Object System.Drawing.Size(560, 140)
$steps.Text = @"
Welcome to KAST. You need: your KAST box, a USB data cable, and OBS installed and running.
No OBS yet? Get it free at https://obsproject.com - install it, then continue here.

1.  Plug the box in with the USB cable, then click Detect box.
2.  Click Flash firmware and wait for the OK lines.
3.  Click Start KAST Companion - it lives in the system tray, near the clock.
4.  In OBS: Tools > WebSocket Server Settings - enable the server,
    click Generate Password, Apply, then Show Connect Info to copy it.
    Then click Set up OBS here and paste that password.
5.  Last: on the box's home screen, tap SETTINGS. Type your Wi-Fi
    name and password and leave the two relay boxes EMPTY - the
    box finds this PC by itself. Tap Save; the status below
    tracks it live and turns green when linked.
"@
$form.Controls.Add($steps)

$portLabel = New-Object System.Windows.Forms.Label
$portLabel.Location = New-Object System.Drawing.Point(16, 158)
$portLabel.Size = New-Object System.Drawing.Size(60, 22)
$portLabel.Text = 'Box port:'
$form.Controls.Add($portLabel)

$portPick = New-Object System.Windows.Forms.ComboBox
$portPick.Location = New-Object System.Drawing.Point(80, 155)
$portPick.Size = New-Object System.Drawing.Size(300, 24)
$portPick.DropDownStyle = 'DropDownList'
$form.Controls.Add($portPick)

function Get-Ports {
    $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $flasher -Action list
    $ports = @()
    foreach ($line in $out) {
        $parts = $line -split "`t"
        if ($parts.Count -eq 3 -and $parts[0] -eq 'PORT') { $ports += "$($parts[1])  ($($parts[2]))" }
    }
    return $ports
}

function Get-SelectedPort {
    if ($portPick.SelectedItem) { return ($portPick.SelectedItem -split ' ')[0] }
    return ''
}

$btnDetect = New-Object System.Windows.Forms.Button
$btnDetect.Text = '1. Detect box'
$btnDetect.Location = New-Object System.Drawing.Point(395, 153)
$btnDetect.Size = New-Object System.Drawing.Size(175, 30)
$form.Controls.Add($btnDetect)

$btnFlash = New-Object System.Windows.Forms.Button
$btnFlash.Text = '2. Flash firmware'
$btnFlash.Location = New-Object System.Drawing.Point(16, 192)
$btnFlash.Size = New-Object System.Drawing.Size(175, 32)
$btnFlash.Enabled = $false
$form.Controls.Add($btnFlash)

$btnTray = New-Object System.Windows.Forms.Button
$btnTray.Text = '3. Start KAST Companion'
$btnTray.Location = New-Object System.Drawing.Point(206, 192)
$btnTray.Size = New-Object System.Drawing.Size(175, 32)
$btnTray.Enabled = $false
$form.Controls.Add($btnTray)

$btnObs = New-Object System.Windows.Forms.Button
$btnObs.Text = '4. Set up OBS'
$btnObs.Location = New-Object System.Drawing.Point(395, 192)
$btnObs.Size = New-Object System.Drawing.Size(175, 32)
$btnObs.Enabled = $false
$form.Controls.Add($btnObs)

# Live box-link status: the wizard carries the user through the last step
# instead of leaving it in a README. Amber while waiting, green on link.
$lblDevice = New-Object System.Windows.Forms.Label
$lblDevice.Location = New-Object System.Drawing.Point(16, 232)
$lblDevice.Size = New-Object System.Drawing.Size(554, 58)
$lblDevice.Font = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold)
$lblDevice.ForeColor = [System.Drawing.Color]::FromArgb(180, 100, 0)
$lblDevice.Text = 'BOX LINK: waiting - on the box, tap SETTINGS. Type your Wi-Fi name and password; leave Relay host and port EMPTY - the box finds this PC by itself. (Those numbers are only needed if it cannot connect: right-click the KAST tray icon on this PC and choose Copy device target - that is what goes there.) This turns green when the box connects.'
$form.Controls.Add($lblDevice)

$script:logBox = New-Object System.Windows.Forms.TextBox
$script:logBox.Location = New-Object System.Drawing.Point(16, 296)
$script:logBox.Size = New-Object System.Drawing.Size(554, 176)
$script:logBox.Multiline = $true
$script:logBox.ReadOnly = $true
$script:logBox.ScrollBars = 'Vertical'
$script:logBox.Font = New-Object System.Drawing.Font('Consolas', 8)
$form.Controls.Add($script:logBox)

$btnRecovery = New-Object System.Windows.Forms.Button
$btnRecovery.Text = 'Advanced Recovery'
$btnRecovery.Location = New-Object System.Drawing.Point(16, 487)
$btnRecovery.Size = New-Object System.Drawing.Size(140, 30)
$form.Controls.Add($btnRecovery)

$btnDriver = New-Object System.Windows.Forms.Button
$btnDriver.Text = 'Install USB driver'
$btnDriver.Location = New-Object System.Drawing.Point(166, 487)
$btnDriver.Size = New-Object System.Drawing.Size(140, 30)
$form.Controls.Add($btnDriver)

$btnClose = New-Object System.Windows.Forms.Button
$btnClose.Text = 'Close'
$btnClose.Location = New-Object System.Drawing.Point(480, 487)
$btnClose.Size = New-Object System.Drawing.Size(90, 30)
$btnClose.DialogResult = [System.Windows.Forms.DialogResult]::OK
$form.CancelButton = $btnClose
$form.Controls.Add($btnClose)

# Poll for a non-localhost established connection on 8765 = the box linked.
$script:deviceLinked = $false
$deviceTimer = New-Object System.Windows.Forms.Timer
$deviceTimer.Interval = 2000
$deviceTimer.Add_Tick({
    if ($script:deviceLinked) { return }
    try {
        $conn = Get-NetTCPConnection -LocalPort 8765 -State Established -ErrorAction SilentlyContinue |
                Where-Object { $_.RemoteAddress -ne '127.0.0.1' -and $_.RemoteAddress -ne '::1' -and $_.RemoteAddress -ne '::ffff:127.0.0.1' } |
                Select-Object -First 1
        if ($conn) {
            $script:deviceLinked = $true
            $lblDevice.ForeColor = [System.Drawing.Color]::FromArgb(0, 130, 60)
            $lblDevice.Text = 'BOX LINK: connected - KAST is ready. Last steps, all from the KAST tray icon (right-click it, near the clock): Authorize with Kick for live chat; Scene Tiles picks which OBS scenes the box controls; Sound/Graphics Library imports your media.'
            Write-Log 'Box connected over Wi-Fi. Setup is complete.'
            $deviceTimer.Stop()
        }
    } catch { }
})
$deviceTimer.Start()

# --- behaviour ----------------------------------------------------------------
$btnDetect.Add_Click({
    $btnDetect.Enabled = $false
    $portPick.Items.Clear()
    $ports = @(Get-Ports)
    if ($ports.Count -eq 0) {
        Write-Log 'No COM ports found. Plug the box in with a USB data cable. If it still does not appear, click Install USB driver below, then Detect again.'
    } else {
        foreach ($p in $ports) { [void]$portPick.Items.Add($p) }
        $portPick.SelectedIndex = 0
        Write-Log "Found $($ports.Count) port(s). Checking for an ESP32-S3 on $(Get-SelectedPort)..."
        if (Invoke-Flasher 'detect' (Get-SelectedPort)) {
            Write-Log 'Box detected. Now click Flash firmware.'
            $btnFlash.Enabled = $true
        } else {
            Write-Log 'That port did not answer as an ESP32-S3. If several ports are listed, pick another and Detect again.'
        }
    }
    $btnDetect.Enabled = $true
})

$btnFlash.Add_Click({
    $btnFlash.Enabled = $false
    if (Invoke-Flasher 'flash' (Get-SelectedPort)) {
        if (Invoke-Flasher 'verify' (Get-SelectedPort)) {
            Write-Log 'Firmware is in and the box booted it. Now click Start KAST Companion.'
            $btnObs.Enabled = $true
            $btnTray.Enabled = $true
        } else {
            Write-Log 'Flash finished but no boot banner was seen. Try Flash firmware once more.'
            $btnFlash.Enabled = $true
        }
    } else {
        Write-Log 'Flash failed. Try another USB port or cable, then click Flash firmware again.'
        $btnFlash.Enabled = $true
    }
})

$btnObs.Add_Click({
    Start-Process powershell.exe -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-STA', '-File', "`"$(Join-Path $companionDir 'setup_obs.ps1')`"") -Wait
    Write-Log 'OBS setup window closed. If it reported success, last step: on the box, join your Wi-Fi (same network as this PC).'
})

$btnTray.Add_Click({
    $trayProc = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
                Where-Object { $_.CommandLine -like '*tray.ps1*' } | Select-Object -First 1
    if ($trayProc) {
        Write-Log 'KAST Companion is already running - look for its icon in the system tray, near the clock (on Windows 11 it may hide under the ^ overflow arrow).'
        return
    }
    # An orphaned relay (tray killed, its node child left behind) still holds
    # 8765 and would fight a fresh tray for the port - clear it first.
    $orphan = Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($orphan) {
        Stop-Process -Id $orphan.OwningProcess -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
        Write-Log 'Cleared a leftover relay process from a closed Companion.'
    }
    Start-Process (Join-Path $companionDir 'KAST Companion.cmd')
    Write-Log 'KAST Companion is starting in the system tray (near the clock). Next in this wizard: click Set up OBS. After that, only one thing is left: on the box touchscreen, tap the top bar > Wi-Fi and join this network.'
})

$btnRecovery.Add_Click({
    $answer = [System.Windows.Forms.MessageBox]::Show(
        "Advanced Recovery erases the ENTIRE flash on the box - firmware, Wi-Fi settings, everything.`r`n`r`nYou will have to flash the firmware again afterwards. Erase everything?",
        'Advanced Recovery', 'YesNo', 'Warning')
    if ($answer -eq [System.Windows.Forms.DialogResult]::Yes) {
        if (Invoke-Flasher 'erase' (Get-SelectedPort)) {
            Write-Log 'Flash erased. Click Flash firmware to reinstall.'
            $btnFlash.Enabled = $true
        }
    }
})

$btnDriver.Add_Click({
    Write-Log 'Installing the CH340 USB-serial driver (Windows will ask for permission)...'
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$flasher`"", '-Action', 'driver') -join ' '
    try {
        Start-Process powershell.exe -ArgumentList $args -Verb RunAs -Wait
    } catch {
        Write-Log 'Driver install was cancelled.'
        return
    }
    Write-Log 'Driver step finished. Unplug and replug the box, then click Detect box again.'
})

$form.Add_Shown({
    $form.Activate()
    # Resume awareness: a re-run (after an accidental close, say) unlocks
    # the steps that are already done instead of forcing the user to redo
    # them. The box-side steps can always be redone with Detect/Flash.
    $resumed = @()
    $trayProc = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
                Where-Object { $_.CommandLine -like '*tray.ps1*' } | Select-Object -First 1
    if ($trayProc) {
        $btnTray.Enabled = $true
        $resumed += 'Companion is already running'
    } elseif (Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue) {
        # orphan relay with no tray: let Start KAST Companion clear and restart it
        $btnTray.Enabled = $true
        $resumed += 'a leftover relay process was found (Companion was closed)'
    }
    $envFile = Join-Path $root 'relay\.env'
    if (Test-Path $envFile) {
        if ((Get-Content $envFile -Raw) -match 'KAST_OBS_ENABLED=1') {
            $btnObs.Enabled = $true
            $resumed += 'OBS is already configured'
        }
    }
    if ($resumed.Count -gt 0) {
        Write-Log ('Picking up where you left off: ' + ($resumed -join '; ') + '.')
        Write-Log 'If the box is not on Wi-Fi yet, that is the only thing left - see BOX LINK below.'
    } else {
        Write-Log 'Plug in the box, then click Detect box.'
    }
})
[void]$form.ShowDialog()
