foreach ($log in 'Microsoft-Windows-CodeIntegrity/Operational', 'Microsoft-Windows-CodeIntegrity/Verbose') {
  Write-Host "=== $log"
  $evts = Get-WinEvent -LogName $log -MaxEvents 10 -ErrorAction SilentlyContinue
  if (-not $evts) { Write-Host '(no events)'; continue }
  foreach ($e in $evts) {
    Write-Host ("{0}  Id {1}" -f $e.TimeCreated, $e.Id)
    $x = [xml]$e.ToXml()
    foreach ($d in $x.Event.EventData.Data) { Write-Host ("   {0} = {1}" -f $d.Name, ($d.'#text' | Out-String).Trim()) }
  }
}
