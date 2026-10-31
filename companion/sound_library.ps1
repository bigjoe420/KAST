# Guided KAST sound library: add, replace, and remove without folders or terminals.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$script:instanceOwned = $false
$script:singleInstance = New-Object System.Threading.Mutex($true, 'KASTSoundLibrarySingleInstance', [ref]$script:instanceOwned)
if (-not $script:instanceOwned) {
    try { (New-Object -ComObject WScript.Shell).AppActivate('KAST Sound Library') | Out-Null } catch {}
    exit 0
}
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$relayDir = Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\relay')
$node = @('C:\Development\node24\current\node.exe', 'node.exe') | Where-Object { $_ -eq 'node.exe' -or (Test-Path $_) } | Select-Object -First 1

function Invoke-SoundCli {
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
$form.Text = 'KAST Sound Library'
$form.Size = New-Object System.Drawing.Size(480, 570)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$label = New-Object System.Windows.Forms.Label
$label.Location = New-Object System.Drawing.Point(18, 14)
$label.Size = New-Object System.Drawing.Size(425, 42)
$label.Text = 'Your 12 sound buttons. Changes automatically sync to OBS and the box.'
$form.Controls.Add($label)
$list = New-Object System.Windows.Forms.ListBox
$list.Location = New-Object System.Drawing.Point(18, 62)
$list.Size = New-Object System.Drawing.Size(425, 350)
$list.Font = New-Object System.Drawing.Font('Segoe UI', 12)
$form.Controls.Add($list)
$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(18, 420)
$status.Size = New-Object System.Drawing.Size(425, 25)
$form.Controls.Add($status)

function Refresh-Sounds {
    $list.Items.Clear()
    $json = Invoke-SoundCli @('sound_command.js', 'list')
    $ids = @((ConvertFrom-Json $json))
    foreach ($id in $ids) { $list.Items.Add([string]$id) | Out-Null }
    $status.Text = "$($ids.Count) of 12 slots used"
}

function Select-AudioFiles {
    param([bool]$Multiple = $true)
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Filter = 'Audio files (*.mp3;*.wav;*.ogg;*.m4a)|*.mp3;*.wav;*.ogg;*.m4a'
    $dialog.Multiselect = $Multiple
    $dialog.Title = if ($Multiple) { 'Choose sounds to add' } else { 'Choose the replacement sound' }
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.FileNames }
    return $null
}

function Add-Sounds {
    $files = Select-AudioFiles $true
    if (-not $files) { return }
    $manifest = Join-Path $relayDir 'sound-import.json.tmp'
    try {
        Set-Content -Path $manifest -Value (ConvertTo-Json @($files)) -Encoding UTF8
        $result = Invoke-SoundCli @('sound_command.js', 'import', $manifest)
        $status.Text = $result
        Start-Sleep -Milliseconds 700
        Refresh-Sounds
    } finally { Remove-Item $manifest -Force -ErrorAction SilentlyContinue }
}

$add = New-Object System.Windows.Forms.Button
$add.Text = 'Add sounds...'
$add.Location = New-Object System.Drawing.Point(18, 460)
$add.Size = New-Object System.Drawing.Size(100, 38)
$add.Add_Click({ try { Add-Sounds } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not add sounds', 'OK', 'Warning') | Out-Null } })
$form.Controls.Add($add)
$replace = New-Object System.Windows.Forms.Button
$replace.Text = 'Replace...'
$replace.Location = New-Object System.Drawing.Point(126, 460)
$replace.Size = New-Object System.Drawing.Size(100, 38)
$replace.Add_Click({
    try {
        if ($list.SelectedIndex -lt 0) { throw 'Select the sound button you want to replace first.' }
        $files = Select-AudioFiles $false
        if (-not $files) { return }
        $id = [string]$list.SelectedItem
        Invoke-SoundCli @('obs_command.js', 'release', $id) | Out-Null
        $manifest = Join-Path $relayDir 'sound-import.json.tmp'
        try {
            Set-Content -Path $manifest -Value (ConvertTo-Json @($files)) -Encoding UTF8
            $result = Invoke-SoundCli @('sound_command.js', 'replace', $id, $manifest)
            $status.Text = $result
            Start-Sleep -Milliseconds 700
            Refresh-Sounds
        } catch {
            try { Invoke-SoundCli @('obs_command.js', 'setup') | Out-Null } catch {}
            throw
        } finally { Remove-Item $manifest -Force -ErrorAction SilentlyContinue }
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not replace sound', 'OK', 'Warning') | Out-Null }
})
$form.Controls.Add($replace)
$remove = New-Object System.Windows.Forms.Button
$remove.Text = 'Remove'
$remove.Location = New-Object System.Drawing.Point(234, 460)
$remove.Size = New-Object System.Drawing.Size(100, 38)
$remove.Add_Click({
    try {
        if ($list.SelectedIndex -lt 0) { throw 'Select the sound button you want to remove first.' }
        $id = [string]$list.SelectedItem
        $answer = [System.Windows.Forms.MessageBox]::Show("Remove '$id' from KAST and OBS? Your original imported file is not changed.", 'Remove sound', 'YesNo', 'Question')
        if ($answer -ne [System.Windows.Forms.DialogResult]::Yes) { return }
        Invoke-SoundCli @('obs_command.js', 'release', $id) | Out-Null
        try {
            $status.Text = Invoke-SoundCli @('sound_command.js', 'remove', $id)
            Start-Sleep -Milliseconds 700
            Refresh-Sounds
        } catch {
            try { Invoke-SoundCli @('obs_command.js', 'setup') | Out-Null } catch {}
            throw
        }
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not remove sound', 'OK', 'Warning') | Out-Null }
})
$form.Controls.Add($remove)
$close = New-Object System.Windows.Forms.Button
$close.Text = 'Close'
$close.Location = New-Object System.Drawing.Point(342, 460)
$close.Size = New-Object System.Drawing.Size(100, 38)
$close.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$form.CancelButton = $close
$form.Controls.Add($close)
$form.Add_Shown({ $form.Activate(); try { Refresh-Sounds } catch { $status.Text = $_.Exception.Message } })
$form.ShowDialog() | Out-Null
