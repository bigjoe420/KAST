# KAST Scene Tiles picker: choose which of YOUR OBS scenes appear as
# buttons on the box. KAST never renames or deletes your scenes.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$script:instanceOwned = $false
$script:singleInstance = New-Object System.Threading.Mutex($true, 'KASTSceneTilesSingleInstance', [ref]$script:instanceOwned)
if (-not $script:instanceOwned) {
    try { (New-Object -ComObject WScript.Shell).AppActivate('KAST Scene Tiles') | Out-Null } catch {}
    exit 0
}
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$relayDir = Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\relay')
$node = @('C:\Development\node24\current\node.exe', 'node.exe') | Where-Object { $_ -eq 'node.exe' -or (Test-Path $_) } | Select-Object -First 1

function Invoke-SceneCli {
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
$form.Text = 'KAST Scene Tiles'
$form.Size = New-Object System.Drawing.Size(480, 560)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$label = New-Object System.Windows.Forms.Label
$label.Location = New-Object System.Drawing.Point(18, 14)
$label.Size = New-Object System.Drawing.Size(425, 60)
$label.Text = 'Check the OBS scenes that should appear as buttons on the box (up to 12). Tapping a tile switches the OBS program scene; the live scene glows green.'
$form.Controls.Add($label)
$list = New-Object System.Windows.Forms.CheckedListBox
$list.Location = New-Object System.Drawing.Point(18, 80)
$list.Size = New-Object System.Drawing.Size(425, 330)
$list.Font = New-Object System.Drawing.Font('Segoe UI', 12)
$list.CheckOnClick = $true
$form.Controls.Add($list)
$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(18, 418)
$status.Size = New-Object System.Drawing.Size(425, 25)
$form.Controls.Add($status)

function Refresh-Scenes {
    $reply = ConvertFrom-Json (Invoke-SceneCli @('scene_command.js', 'list'))
    $list.Items.Clear()
    foreach ($scene in @($reply.scenes)) {
        $checked = @($reply.selected) -contains [string]$scene
        $list.Items.Add([string]$scene, $checked) | Out-Null
    }
    $status.Text = "$(@($reply.selected).Count) of 12 tiles selected"
}

$save = New-Object System.Windows.Forms.Button
$save.Text = 'Save tiles'
$save.Location = New-Object System.Drawing.Point(18, 456)
$save.Size = New-Object System.Drawing.Size(120, 38)
$save.Add_Click({
    try {
        $picked = @($list.CheckedItems | ForEach-Object { [string]$_ })
        $manifest = Join-Path $relayDir 'scene-tiles.json.tmp'
        try {
            Set-Content -Path $manifest -Value (ConvertTo-Json @($picked)) -Encoding UTF8
            $status.Text = Invoke-SceneCli @('scene_command.js', 'set', $manifest)
        } finally { Remove-Item $manifest -Force -ErrorAction SilentlyContinue }
    } catch { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Could not save scene tiles', 'OK', 'Warning') | Out-Null }
})
$form.Controls.Add($save)

$refresh = New-Object System.Windows.Forms.Button
$refresh.Text = 'Reload from OBS'
$refresh.Location = New-Object System.Drawing.Point(150, 456)
$refresh.Size = New-Object System.Drawing.Size(120, 38)
$refresh.Add_Click({ try { Refresh-Scenes } catch { $status.Text = $_.Exception.Message } })
$form.Controls.Add($refresh)

$close = New-Object System.Windows.Forms.Button
$close.Text = 'Close'
$close.Location = New-Object System.Drawing.Point(343, 456)
$close.Size = New-Object System.Drawing.Size(100, 38)
$close.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$form.CancelButton = $close
$form.Controls.Add($close)
$form.Add_Shown({ $form.Activate(); try { Refresh-Scenes } catch { $status.Text = $_.Exception.Message } })
$form.ShowDialog() | Out-Null
