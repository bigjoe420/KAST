$files = @('installer\kast_flash.ps1', 'companion\first_run.ps1', 'companion\setup_obs.ps1', 'companion\sound_library.ps1', 'companion\scene_tiles.ps1', 'companion\graphics_library.ps1', 'companion\tray.ps1')
$bad = $false
foreach ($f in $files) {
    $errs = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path (Split-Path -Parent $PSScriptRoot) $f), [ref]$null, [ref]$errs)
    if ($errs -and $errs.Count) { $bad = $true; foreach ($e in $errs) { Write-Host "$f :: $($e.Message)" } }
    else { Write-Host "$f OK" }
}
exit ([int]$bad)
