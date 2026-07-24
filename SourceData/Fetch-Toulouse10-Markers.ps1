# Recupere les reperes de la ville sur 10 x 10 km -> toulouse10_markers.json
#  - metro (stations + entrees), eglises, mairies : comme Fetch-Markers.ps1
#  - NOUVEAU : zones (place=suburb -> "district") et quartiers
#    (place=quarter|neighbourhood -> "quarter") pour les indications demandees
$ErrorActionPreference = 'Stop'
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

$Lat0 = 43.6045; $Lon0 = 1.4442
$MPerLat = 110540.0
$MPerLon = 111320.0 * [Math]::Cos($Lat0 * [Math]::PI / 180)
$HalfM = 5000.0
$DLat = $HalfM / $MPerLat; $DLon = $HalfM / $MPerLon
$S = $Lat0 - $DLat; $N = $Lat0 + $DLat; $W = $Lon0 - $DLon; $E = $Lon0 + $DLon

$Q = @"
[out:json][timeout:120];
(
  node["station"="subway"]($S,$W,$N,$E);
  node["railway"="subway_entrance"]($S,$W,$N,$E);
  node["amenity"~"place_of_worship|townhall"]($S,$W,$N,$E);
  way["amenity"~"place_of_worship|townhall"]($S,$W,$N,$E);
  node["place"~"suburb|quarter|neighbourhood"]($S,$W,$N,$E);
);
out center;
"@
$Body = 'data=' + [Uri]::EscapeDataString($Q)
$Mirrors = @('https://overpass.openstreetmap.fr/api/interpreter',
	'https://overpass.kumi.systems/api/interpreter',
	'https://overpass-api.de/api/interpreter')
$R = $null
for ($Try = 0; $Try -lt 6; $Try++) {
	try {
		$R = Invoke-RestMethod -Uri $Mirrors[$Try % $Mirrors.Count] -Method Post -Body $Body -ContentType 'application/x-www-form-urlencoded' -UserAgent 'CityLab-DroneFPV/1.0 (contact: mourradmohsen@gmail.com)' -TimeoutSec 180
		break
	} catch { Write-Host "  echec ($Try), retry dans 15 s"; Start-Sleep 15 }
}
if (-not $R) { throw 'Overpass : indisponible apres 6 essais.' }

$Markers = New-Object System.Collections.Generic.List[object]
$Counts = @{}
foreach ($El in $R.elements) {
	$Lat = if ($El.center) { $El.center.lat } else { $El.lat }
	$Lon = if ($El.center) { $El.center.lon } else { $El.lon }
	$X = [Math]::Round(($Lon - $Lon0) * $MPerLon, 1)
	# NORD = -Y (chiralite Unreal main gauche — cf. Fetch-Toulouse10.ps1).
	$Y = [Math]::Round(($Lat0 - $Lat) * $MPerLat, 1)
	$Place = "$($El.tags.place)"
	$Kind = if ($El.tags.station -eq 'subway') { 'metro' }
		elseif ($El.tags.railway -eq 'subway_entrance') { 'metro_e' }
		elseif ($El.tags.amenity -eq 'townhall') { 'townhall' }
		elseif ($El.tags.amenity -eq 'place_of_worship') { 'church' }
		elseif ($Place -eq 'suburb') { 'district' }
		elseif ($Place -in @('quarter', 'neighbourhood')) { 'quarter' }
		else { $null }
	if (-not $Kind) { continue }
	$Name = "$($El.tags.name)"
	# Les zones/quartiers sans nom n'indiquent rien : on les saute.
	if (($Kind -in @('district', 'quarter')) -and -not $Name) { continue }
	$Markers.Add(@{ x = $X; y = $Y; k = $Kind; n = $Name })
	$Counts[$Kind] = 1 + [int]$Counts[$Kind]
}
$Path = Join-Path $PSScriptRoot 'toulouse10_markers.json'
@{ markers = $Markers } | ConvertTo-Json -Depth 4 -Compress | Out-File -FilePath $Path -Encoding utf8
"Marqueurs : $($Markers.Count) -> $Path"
$Counts.GetEnumerator() | Sort-Object Name | ForEach-Object { "  $($_.Name) : $($_.Value)" }
