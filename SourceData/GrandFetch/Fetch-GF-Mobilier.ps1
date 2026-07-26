# GRAND FETCH - complement MOBILIER URBAIN (extension 26/07) : bancs, poubelles,
# fontaines, abribus, stations-service, feux tricolores et autres elements du
# quotidien. Source OSM/Overpass (ODbL) - couverture crowdsourcee INDICATIVE.
# v2 : UNE requete union (1 creneau serveur au lieu de 16 - les miroirs saturent),
# classification par tags cote client, -UseBasicParsing (sans lui, PS 5.1 passe
# par le moteur IE qui tente des invites -> echec en session non interactive).
# Sortie : mobilier.json {"items":[{"k":"bench","x":..,"y":..,"n":"nom"},...]}
$ErrorActionPreference = 'Stop'
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

$Lat0 = 43.6045; $Lon0 = 1.4442
$MPerLat = 110540.0
$MPerLon = 111320.0 * [Math]::Cos($Lat0 * [Math]::PI / 180)
$HalfM = 5000.0
$DLat = $HalfM / $MPerLat; $DLon = $HalfM / $MPerLon
$S = ($Lat0 - $DLat).ToString('0.########', $Inv); $N = ($Lat0 + $DLat).ToString('0.########', $Inv)
$W = ($Lon0 - $DLon).ToString('0.########', $Inv); $E = ($Lon0 + $DLon).ToString('0.########', $Inv)

function F([double]$V) { return $V.ToString('0.##', $Inv) }
function ToLocalX([double]$Lon) { return [Math]::Round(($Lon - $Lon0) * $MPerLon, 2) }
# NORD = -Y (chiralite Unreal main gauche - cf. Fetch-Toulouse10.ps1).
function ToLocalY([double]$Lat) { return [Math]::Round(($Lat0 - $Lat) * $MPerLat, 2) }

$Bb = "($S,$W,$N,$E)"
$Q = @"
[out:json][timeout:300];
(
  node["amenity"="bench"]$Bb;
  node["amenity"="waste_basket"]$Bb;
  nwr["amenity"="recycling"]$Bb;
  nwr["amenity"="fountain"]$Bb;
  node["amenity"="drinking_water"]$Bb;
  node["highway"="bus_stop"]$Bb;
  nwr["amenity"="shelter"]$Bb;
  nwr["amenity"="bicycle_parking"]$Bb;
  nwr["amenity"="toilets"]$Bb;
  node["amenity"="post_box"]$Bb;
  node["emergency"="fire_hydrant"]$Bb;
  node["leisure"="picnic_table"]$Bb;
  nwr["amenity"="fuel"]$Bb;
  nwr["amenity"="charging_station"]$Bb;
  nwr["amenity"="car_wash"]$Bb;
  node["highway"="traffic_signals"]$Bb;
);
out center;
"@
$Body = 'data=' + [Uri]::EscapeDataString($Q)

$Mirrors = @('https://overpass-api.de/api/interpreter',
	'https://overpass.kumi.systems/api/interpreter',
	'https://overpass.openstreetmap.fr/api/interpreter',
	'https://overpass.private.coffee/api/interpreter')

Write-Host 'Overpass : requete UNION unique (16 categories)...'
$O = $null
for ($Try = 0; $Try -lt 12; $Try++) {
	$Mirror = $Mirrors[$Try % $Mirrors.Count]
	try {
		$Resp = Invoke-WebRequest -Uri $Mirror -Method Post -Body $Body -UseBasicParsing `
			-ContentType 'application/x-www-form-urlencoded' `
			-UserAgent 'CityLab-DroneFPV/1.0 (contact: mourradmohsen@gmail.com)' -TimeoutSec 320
		$O = [System.Text.Encoding]::UTF8.GetString($Resp.RawContentStream.ToArray()) | ConvertFrom-Json
		Write-Host "  reussi sur $Mirror ($($O.elements.Count) elements bruts)"
		break
	} catch {
		Write-Host "  echec $Mirror : $($_.Exception.Message) - retry 60 s"
		Start-Sleep 60
	}
}
if (-not $O) { throw 'Overpass : union irrecuperable apres 12 tentatives.' }

# Classification par tags (l'union melange tout).
function KindOf($T) {
	if ($T.amenity -eq 'bench') { return 'bench' }
	if ($T.amenity -eq 'waste_basket') { return 'waste_basket' }
	if ($T.amenity -eq 'recycling') { return 'recycling' }
	if ($T.amenity -eq 'fountain') { return 'fountain' }
	if ($T.amenity -eq 'drinking_water') { return 'drinking_water' }
	if ($T.highway -eq 'bus_stop') { return 'bus_stop' }
	if ($T.amenity -eq 'shelter') { return 'shelter' }
	if ($T.amenity -eq 'bicycle_parking') { return 'bicycle_parking' }
	if ($T.amenity -eq 'toilets') { return 'toilets' }
	if ($T.amenity -eq 'post_box') { return 'post_box' }
	if ($T.emergency -eq 'fire_hydrant') { return 'hydrant' }
	if ($T.leisure -eq 'picnic_table') { return 'picnic_table' }
	if ($T.amenity -eq 'fuel') { return 'fuel' }
	if ($T.amenity -eq 'charging_station') { return 'charging_station' }
	if ($T.amenity -eq 'car_wash') { return 'car_wash' }
	if ($T.highway -eq 'traffic_signals') { return 'traffic_signals' }
	return $null
}

$Sb = New-Object System.Text.StringBuilder
$NbTot = 0
$Counts = @{}
$Seen = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($El in $O.elements) {
	if (-not $Seen.Add("$($El.type)/$($El.id)")) { continue }
	$Kind = if ($El.tags) { KindOf $El.tags } else { $null }
	if (-not $Kind) { continue }
	$Lat = if ($null -ne $El.lat) { $El.lat } elseif ($El.center) { $El.center.lat } else { $null }
	$Lon = if ($null -ne $El.lon) { $El.lon } elseif ($El.center) { $El.center.lon } else { $null }
	if ($null -eq $Lat) { continue }
	if ($NbTot -gt 0) { [void]$Sb.Append(',') }
	[void]$Sb.Append('{"k":"').Append($Kind).Append('","x":').Append((F (ToLocalX $Lon)))
	[void]$Sb.Append(',"y":').Append((F (ToLocalY $Lat)))
	if ($El.tags.name) {
		$Nm = "$($El.tags.name)".Replace('\', '\\').Replace('"', '\"')
		[void]$Sb.Append(',"n":"').Append($Nm).Append('"')
	}
	if ($Kind -eq 'fuel' -and $El.tags.brand) {
		$Br = "$($El.tags.brand)".Replace('\', '\\').Replace('"', '\"')
		[void]$Sb.Append(',"brand":"').Append($Br).Append('"')
	}
	[void]$Sb.Append('}')
	$NbTot++
	$Counts[$Kind] = 1 + $(if ($Counts.ContainsKey($Kind)) { $Counts[$Kind] } else { 0 })
}

foreach ($K in ($Counts.Keys | Sort-Object)) { Write-Host ("  {0,-18} : {1}" -f $K, $Counts[$K]) }
$Out = '{"source":"OSM Overpass (ODbL) - mobilier urbain, couverture INDICATIVE","items":[' + $Sb.ToString() + ']}'
$Path = Join-Path $PSScriptRoot 'mobilier.json'
[System.IO.File]::WriteAllText($Path, $Out, (New-Object System.Text.UTF8Encoding $false))
Write-Host "FIN : $NbTot elements -> $Path ($([Math]::Round((Get-Item $Path).Length/1KB,0)) Ko)"