$mp = Get-MpComputerStatus -ErrorAction SilentlyContinue
Write-Host "SmartAppControlState: $($mp.SmartAppControlState)  (0=off 1=evaluate 2=enforced)"
Write-Host "AMServiceEnabled: $($mp.AMServiceEnabled)"
$al = Get-AppLockerPolicy -Effective -Xml -ErrorAction SilentlyContinue
if ($al) {
  Write-Host "--- effective AppLocker rules:"
  Write-Host $al
} else {
  Write-Host "no effective AppLocker policy (or access denied)"
}
