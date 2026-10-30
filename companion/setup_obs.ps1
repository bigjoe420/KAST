# Direct KAST OBS setup shortcut. Safe to run while Companion is running.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$relayDir = Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\relay')
$form = New-Object System.Windows.Forms.Form
$form.Text = 'Set up KAST with OBS'
$form.Size = New-Object System.Drawing.Size(540, 300)
$form.StartPosition = 'CenterScreen'
$form.Topmost = $true
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$label = New-Object System.Windows.Forms.Label
$label.Location = New-Object System.Drawing.Point(18, 16)
$label.Size = New-Object System.Drawing.Size(490, 85)
$label.Text = "OBS WebSocket is enabled. Now enter its password.`r`n(OBS > Tools > WebSocket Server Settings > Show Password)`r`n`r`nKAST adds only its own SFX scene and sources."
$form.Controls.Add($label)
$pw = New-Object System.Windows.Forms.TextBox
$pw.Location = New-Object System.Drawing.Point(18, 112)
$pw.Size = New-Object System.Drawing.Size(490, 24)
$pw.UseSystemPasswordChar = $true
$form.Controls.Add($pw)
$ok = New-Object System.Windows.Forms.Button
$ok.Text = 'Save and connect'
$ok.Location = New-Object System.Drawing.Point(230, 165)
$ok.Size = New-Object System.Drawing.Size(130, 34)
$ok.DialogResult = [System.Windows.Forms.DialogResult]::OK
$form.AcceptButton = $ok
$form.Controls.Add($ok)
$cancel = New-Object System.Windows.Forms.Button
$cancel.Text = 'Cancel'
$cancel.Location = New-Object System.Drawing.Point(370, 165)
$cancel.Size = New-Object System.Drawing.Size(90, 34)
$cancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$form.CancelButton = $cancel
$form.Controls.Add($cancel)
$form.Add_Shown({ $form.Activate(); $pw.Focus() })
if ($form.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }

$envFile = Join-Path $relayDir '.env'
$lines = if (Test-Path $envFile) { @(Get-Content $envFile) } else { @() }
$values = @{ KAST_OBS_ENABLED = '1'; KAST_OBS_URL = 'ws://127.0.0.1:4455'; KAST_OBS_PASSWORD = $pw.Text }
foreach ($key in $values.Keys) {
    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match "^\s*$key\s*=") { $lines[$i] = "$key=$($values[$key])"; $found = $true }
    }
    if (-not $found) { $lines += "$key=$($values[$key])" }
}
Set-Content -Path $envFile -Value $lines -Encoding UTF8

# Companion notices its relay child stop and starts it with the new settings.
$listener = Get-NetTCPConnection -LocalPort 8765 -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
if ($listener) { Stop-Process -Id $listener.OwningProcess -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 3

$node = @('C:\Development\node24\current\node.exe', 'node.exe') | Where-Object { $_ -eq 'node.exe' -or (Test-Path $_) } | Select-Object -First 1
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $node
$psi.Arguments = 'obs_command.js setup'
$psi.WorkingDirectory = $relayDir.Path
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)
$stdout = $p.StandardOutput.ReadToEnd().Trim()
$stderr = $p.StandardError.ReadToEnd().Trim()
$p.WaitForExit()
if ($p.ExitCode -eq 0) {
    [System.Windows.Forms.MessageBox]::Show("Connected. KAST SFX is ready.`r`n`r`nNext: right-click KAST Companion and choose Test OBS sound.", 'KAST OBS setup complete', 'OK', 'Information') | Out-Null
} else {
    [System.Windows.Forms.MessageBox]::Show($stderr, 'KAST OBS setup needs attention', 'OK', 'Warning') | Out-Null
}
