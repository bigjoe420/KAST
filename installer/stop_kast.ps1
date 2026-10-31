# stop_kast.ps1 — extracted to {tmp} by KAST-Setup before install.
# Stops any running KAST tray/relay belonging to the target install folder so
# its files (node.exe, relay scripts) are not locked during overwrite.
# Matches only the known KAST process images — never the installer itself,
# whose own command line also mentions the install folder (via /DIR=).
param([Parameter(Mandatory = $true)][string]$AppDir)
$images = 'node.exe', 'powershell.exe', 'pwsh.exe', 'cmd.exe'
Get-CimInstance Win32_Process |
    Where-Object {
        $_.ProcessId -ne $PID -and
        $images -contains $_.Name -and
        $_.CommandLine -and
        $_.CommandLine.Contains($AppDir)
    } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
exit 0
