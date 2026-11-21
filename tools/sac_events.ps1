$evts = Get-WinEvent -LogName 'Microsoft-Windows-Windows Defender/Operational' -MaxEvents 400 -ErrorAction SilentlyContinue |
  Where-Object { $_.Id -in 1121, 1122, 5007 } | Select-Object -First 12
foreach ($e in $evts) {
  Write-Host ("{0}  Id {1}" -f $e.TimeCreated, $e.Id)
  $x = [xml]$e.ToXml()
  foreach ($d in $x.Event.EventData.Data) { Write-Host ("   {0} = {1}" -f $d.Name, $d.'#text') }
  Write-Host ""
}
