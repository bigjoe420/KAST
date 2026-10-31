# Guided KAST graphics library: add, replace, remove, bind a sound, set
# visible time — no folders or terminals. Mirrors sound_library.ps1.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$script:instanceOwned = $false
$script:singleInstance = New-Object System.Threading.Mutex($true, 'KASTGraphicsLibrarySingleInstance', [ref]$script:instanceOwned)
if (-not $script:instanceOwned) {
    try { (New-Object -ComObject WScript.Shell).AppActivate('KAST Graphics Library') | Out-Null } catch {}
    exit 0
}
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$relayDir = Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\relay')
$node = @((Join-Path $PSScriptRoot '..\node24\current\node.exe'), 'C:\Development\node24\current\node.exe', 'node.exe') | Where-Object { $_ -eq 'node.exe' -or (Test-Path $_) } | Select-Object -First 1

function Invoke-GfxCli {
    param([string[]]$Arguments)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $node
    $psi.Arguments = ($Arguments | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' '
    $psi.WorkingDirectory = $relayDir.Path
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEnd().Trim()
    $stderr = $p.StandardError.ReadToEnd().Trim()
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { throw $stderr }
    return $stdout
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'KAST Graphics Library'
$form.Size = New-Object System.Drawing.Size(520, 640)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$label = New-Object System.Windows.Forms.Label
$label.Location = New-Object System.Drawing.Point(18, 14)
$label.Size = New-Object System.Drawing.Size(465, 42)
$label.Text = 'Your 12 graphic buttons. Each overlays the live scene; a bound sound plays with it. Changes sync to OBS and the box automatically.'
$form.Controls.Add($label)
$list = New-Object System.Windows.Forms.ListBox
$list.Location = New-Object System.Drawing.Point(18, 62)
$list.Size = New-Object System.Drawing.Size(465, 330)
$list.Font = New-Object System.Drawing.Font('Segoe UI', 12)
$form.Controls.Add($list)
$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(18, 400)
$status.Size = New-Object System.Drawing.Size(465, 25)
$form.Controls.Add($status)

# Per-graphic behavior row: optional sound + visible seconds
$soundLabel = New-Object System.Windows.Forms.Label
$soundLabel.Text = 'Sound'
$soundLabel.Location = New-Object System.Drawing.Point(18, 436)
$soundLabel.Size = New-Object System.Drawing.Size(55, 24)
$form.Controls.Add($soundLabel)
$soundBox = New-Object System.Windows.Forms.ComboBox
$soundBox.Location = New-Object System.Drawing.Point(76, 432)
$soundBox.Size = New-Object System.Drawing.Size(170, 24)
$soundBox.DropDownStyle = 'DropDownList'
$form.Controls.Add($soundBox)
$durLabel = New-Object System.Windows.Forms.Label
$durLabel.Text = 'Seconds on screen'
$durLabel.Location = New-Object System.Drawing.Point(256, 436)
$durLabel.Size = New-Object System.Drawing.Size(115, 24)
$form.Controls.Add($durLabel)
$durBox = New-Object System.Windows.Forms.TextBox
$durBox.Location = New-Object System.Drawing.Point(374, 432)
$durBox.Size = New-Object System.Drawing.Size(60, 24)
$durBox.Text = '5'
$form.Controls.Add($durBox)
$applyBind = New-Object System.Windows.Forms.Button
$applyBind.Text = 'Apply'
$applyBind.Location = New-Object System.Drawing.Point(442, 430)
$applyBind.Size = New-Object System.Drawing.Size(60, 28)
$form.Controls.Add($applyBind)

$script:entries = @()

function Refresh-Graphics {
    $list.Items.Clear()
    $json = Invoke-GfxCli @('gfx_command.js', 'list')
    $script:entries = @((ConvertFrom-Json $json))
    foreach ($entry in $script:entries) {
        $suffix = if ($entry.sound) { "  (+ $($entry.sound))" } else { '' }
        $list.Items.Add("$($entry.id)$suffix") | Out-Null
    }
    $status.Text = "$($script:entries.Count) of 12 slots used"
    $soundBox.Items.Clear()
    $soundBox.Items.Add('(no sound)') | Out-Null
    foreach ($sid in @(ConvertFrom-Json (Invoke-GfxCli @('sound_command.js', 'list')))) {
        $soundBox.Items.Add([string]$sid) | Out-Null
    }
    $soundBox.SelectedIndex = 0
}

$list.Add_SelectedIndexChanged({
    $i = $list.SelectedIndex
    if ($i -lt 0 -or $i -ge $script:entries.Count) { return }
    $entry = $script:entries[$i]
    $soundBox.SelectedItem = if ($entry.sound) { [string]$entry.sound } else { '(no sound)' }
    $durBox.Text = [string][int]($entry.durationMs / 1000)
})

$applyBind.Add_Click({
    try {
        $i = $list.SelectedIndex
        if ($i -lt 0 -or $i -ge $script:entries.Count) { throw 'Select a graphic first.' }
        $seconds = [double]::Parse($durBox.Text)
        if ($seconds -lt 0.5 -or $seconds -gt 60) { throw 'Seconds on screen must be between 0.5 and 60.' }
        $sound = if ($soundBox.SelectedItem -eq '(no sound)') { 'none' } else { [string]$soundBox.SelectedItem }
        $status.Text = Invoke-GfxCli @('gfx_command.js', 'bind', $script:entries[$i].id, $sound, [string][int]($seconds * 1000))
        Start-Sleep -Milliseconds 400
        Refresh-Graphics
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not apply settings', 'OK', 'Warning') | Out-Null }
})

function Select-GraphicFiles {
    param([bool]$Multiple = $true)
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Filter = 'Graphics (*.mp4;*.webm;*.mov;*.gif;*.png;*.jpg)|*.mp4;*.webm;*.mov;*.gif;*.png;*.jpg'
    $dialog.Multiselect = $Multiple
    $dialog.Title = if ($Multiple) { 'Choose graphics to add' } else { 'Choose the replacement graphic' }
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.FileNames }
    return $null
}

$add = New-Object System.Windows.Forms.Button
$add.Text = 'Add graphics...'
$add.Location = New-Object System.Drawing.Point(18, 478)
$add.Size = New-Object System.Drawing.Size(112, 38)
$add.Add_Click({
    try {
        $files = Select-GraphicFiles $true
        if (-not $files) { return }
        $manifest = Join-Path $relayDir 'gfx-import.json.tmp'
        try {
            Set-Content -Path $manifest -Value (ConvertTo-Json @($files)) -Encoding UTF8
            $status.Text = Invoke-GfxCli @('gfx_command.js', 'import', $manifest)
            Start-Sleep -Milliseconds 700
            Refresh-Graphics
        } finally { Remove-Item $manifest -Force -ErrorAction SilentlyContinue }
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not add graphics', 'OK', 'Warning') | Out-Null }
})
$form.Controls.Add($add)

$replace = New-Object System.Windows.Forms.Button
$replace.Text = 'Replace...'
$replace.Location = New-Object System.Drawing.Point(138, 478)
$replace.Size = New-Object System.Drawing.Size(100, 38)
$replace.Add_Click({
    try {
        if ($list.SelectedIndex -lt 0) { throw 'Select the graphic button you want to replace first.' }
        $files = Select-GraphicFiles $false
        if (-not $files) { return }
        $id = $script:entries[$list.SelectedIndex].id
        Invoke-GfxCli @('obs_command.js', 'gfxrelease', $id) | Out-Null
        $manifest = Join-Path $relayDir 'gfx-import.json.tmp'
        try {
            Set-Content -Path $manifest -Value (ConvertTo-Json @($files)) -Encoding UTF8
            $status.Text = Invoke-GfxCli @('gfx_command.js', 'replace', $id, $manifest)
            Start-Sleep -Milliseconds 700
            Refresh-Graphics
        } catch {
            try { Invoke-GfxCli @('obs_command.js', 'setup') | Out-Null } catch {}
            throw
        } finally { Remove-Item $manifest -Force -ErrorAction SilentlyContinue }
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not replace graphic', 'OK', 'Warning') | Out-Null }
})
$form.Controls.Add($replace)

$remove = New-Object System.Windows.Forms.Button
$remove.Text = 'Remove'
$remove.Location = New-Object System.Drawing.Point(246, 478)
$remove.Size = New-Object System.Drawing.Size(100, 38)
$remove.Add_Click({
    try {
        if ($list.SelectedIndex -lt 0) { throw 'Select the graphic you want to remove first.' }
        $id = $script:entries[$list.SelectedIndex].id
        $answer = [System.Windows.Forms.MessageBox]::Show("Remove '$id' from KAST and OBS? Your original imported file is not changed.", 'Remove graphic', 'YesNo', 'Question')
        if ($answer -ne [System.Windows.Forms.DialogResult]::Yes) { return }
        Invoke-GfxCli @('obs_command.js', 'gfxrelease', $id) | Out-Null
        try {
            $status.Text = Invoke-GfxCli @('gfx_command.js', 'remove', $id)
            Start-Sleep -Milliseconds 700
            Refresh-Graphics
        } catch {
            try { Invoke-GfxCli @('obs_command.js', 'setup') | Out-Null } catch {}
            throw
        }
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not remove graphic', 'OK', 'Warning') | Out-Null }
})
$form.Controls.Add($remove)

$test = New-Object System.Windows.Forms.Button
$test.Text = 'Test on OBS'
$test.Location = New-Object System.Drawing.Point(354, 478)
$test.Size = New-Object System.Drawing.Size(100, 38)
$test.Add_Click({
    try {
        if ($list.SelectedIndex -lt 0) { throw 'Select a graphic first.' }
        Invoke-GfxCli @('obs_command.js', 'gfxplay', $script:entries[$list.SelectedIndex].id) | Out-Null
        $status.Text = 'Playing on the live OBS scene.'
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not play graphic', 'OK', 'Warning') | Out-Null }
})
$form.Controls.Add($test)

$close = New-Object System.Windows.Forms.Button
$close.Text = 'Close'
$close.Location = New-Object System.Drawing.Point(383, 532)
$close.Size = New-Object System.Drawing.Size(100, 38)
$close.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$form.CancelButton = $close
$form.Controls.Add($close)
$form.Add_Shown({ $form.Activate(); try { Refresh-Graphics } catch { $status.Text = $_.Exception.Message } })
$form.ShowDialog() | Out-Null
