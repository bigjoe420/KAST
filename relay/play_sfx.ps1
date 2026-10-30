param(
    [Parameter(Mandatory = $true)][string]$Path
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class KastMci {
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
    public static extern int mciSendString(string command, StringBuilder result,
        int resultLength, IntPtr callback);
}
'@

$alias = "kast_sfx_$PID"
$result = New-Object System.Text.StringBuilder 256

function Invoke-Mci([string]$Command) {
    $script:result.Clear() | Out-Null
    $rc = [KastMci]::mciSendString($Command, $script:result, 256, [IntPtr]::Zero)
    if ($rc -ne 0) {
        throw "Windows audio command failed ($rc): $Command"
    }
    return $script:result.ToString()
}

try {
    Invoke-Mci "open `"$Path`" type mpegvideo alias $alias" | Out-Null
    Invoke-Mci "play $alias" | Out-Null
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        Start-Sleep -Milliseconds 100
        $mode = Invoke-Mci "status $alias mode"
        if ($timer.Elapsed.TotalSeconds -gt 60) {
            throw "Windows audio playback timed out"
        }
    } while ($mode -eq "playing")
}
finally {
    Invoke-Mci "close $alias" | Out-Null
}
