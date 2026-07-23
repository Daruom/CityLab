# Recupere les reperes visuels du quartier (metro, eglises, mairie) -> capitole_markers.json
$ErrorActionPreference = 'Stop'
$Lat0 = 43.6045; $Lon0 = 1.4442
$S = 43.6018; $N = 43.6072; $W = 1.4405; $E = 1.4479
$MPerLat = 110540.0
$MPerLon = 111320.0 * [Math]::Cos($Lat0 * [Math]::PI / 180)

$Q = @"
[out:json][timeout:90];
(
  node["station"="subway"]($S,$W,$N,$E);
  node["railway"="subway_entrance"]($S,$W,$N,$E);
  node["amenity"~"place_of_worship|townhall"]($S,$W,$N,$E);
  way["amenity"~"place_of_worship|townhall"]($S,$W,$N,$E);
);
out center;
"@
$Body = 'data=' + [Uri]::EscapeDataString($Q)
$R = Invoke-RestMethod -Uri 'https://overpass.kumi.systems/api/interpreter' -Method Post -Body $Body -ContentType 'application/x-www-form-urlencoded' -UserAgent 'CityLab-DroneFPV/1.0 (contact: mourradmohsen@gmail.com)' -TimeoutSec 150

$Markers = New-Object System.Collections.Generic.List[object]
foreach ($El in $R.elements) {
	$Lat = if ($El.center) { $El.center.lat } else { $El.lat }
	$Lon = if ($El.center) { $El.center.lon } else { $El.lon }
	$X = [Math]::Round(($Lon - $Lon0) * $MPerLon, 1)
	$Y = [Math]::Round(($Lat - $Lat0) * $MPerLat, 1)
	$Kind = if ($El.tags.station -eq 'subway') { 'metro' }
		elseif ($El.tags.railway -eq 'subway_entrance') { 'metro_e' }
		elseif ($El.tags.amenity -eq 'townhall') { 'townhall' }
		else { 'church' }
	$Name = "$($El.tags.name)"
	$Markers.Add(@{ x = $X; y = $Y; k = $Kind; n = $Name })
}
$Path = Join-Path $PSScriptRoot 'capitole_markers.json'
@{ markers = $Markers } | ConvertTo-Json -Depth 4 -Compress | Out-File -FilePath $Path -Encoding utf8
"Marqueurs : $($Markers.Count) -> $Path"
$Markers | ForEach-Object { "  $($_.k) : $($_.n)" }
