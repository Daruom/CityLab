# GRAND FETCH - complement MOBILIER URBAIN (demande utilisateur 26/07, extension du
# perimetre) : bancs, poubelles, fontaines, abribus, stations-service et autres
# elements du quotidien. Source OSM/Overpass (ODbL) - couverture crowdsourcee,
# INDICATIVE (dense au centre, lacunaire en peripherie), a completer par regles PCG.
# Sortie : mobilier.json {"items":[{"k":"bench","x":..,"y":..,"n":"nom eventuel"},...]}
$ErrorActionPreference = 'Stop'
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

$Lat0 = 43.6045; $Lon0 = 1.4442
$MPerLat = 110540.0
$MPerLon = 111320.0 * [Math]::Cos($Lat0 * [Math]::PI / 180)
$HalfM = 5000.0
$DLat = $HalfM / $MPerLat; $DLon = $HalfM / $MPerLon
$S = $Lat0 - $DLat; $N = $Lat0 + $DLat; $W = $Lon0 - $DLon; $E = $Lon0 + $DLon

function F([double]$V) { return $V.ToString('0.##', $Inv) }
function ToLocalX([double]$Lon) { return [Math]::Round(($Lon - $Lon0) * $MPerLon, 2) }
# NORD = -Y (chiralite Unreal main gauche - cf. Fetch-Toulouse10.ps1).
function ToLocalY([double]$Lat) { return [Math]::Round(($Lat0 - $Lat) * $MPerLat, 2) }

$Mirrors = @('https://overpass.openstreetmap.fr/api/interpreter',
	'https://overpass.kumi.systems/api/interpreter',
	'https://overpass-api.de/api/interpreter')

# Cle de sortie <- filtre OSM. Ways acceptes (centre de l'emprise via out center).
$Kinds = @(
	@('bench',            'node["amenity"="bench"]'),
	@('waste_basket',     'node["amenity"="waste_basket"]'),
	@('recycling',        'nwr["amenity"="recycling"]'),
	@('fountain',         'nwr["amenity"="fountain"]'),
	@('drinking_water',   'node["amenity"="drinking_water"]'),
	@('bus_stop',         'node["highway"="bus_stop"]'),
	@('shelter',          'nwr["amenity"="shelter"]'),
	@('bicycle_parking',  'nwr["amenity"="bicycle_parking"]'),
	@('toilets',          'nwr["amenity"="toilets"]'),
	@('post_box',         'node["amenity"="post_box"]'),
	@('hydrant',          'node["emergency"="fire_hydrant"]'),
	@('picnic_table',     'node["leisure"="picnic_table"]'),
	@('fuel',             'nwr["amenity"="fuel"]'),
	@('charging_station', 'nwr["amenity"="charging_station"]'),
	@('car_wash',         'nwr["amenity"="car_wash"]')
)

$Sb = New-Object System.Text.StringBuilder
$NbTot = 0
$Counts = @{}
$Seen = New-Object 'System.Collections.Generic.HashSet[string]'
# Bbox en InvariantCulture : l'interpolation PS directe met des VIRGULES francaises
# dans les doubles -> requete Overpass malformee (echec sur tous les miroirs).
$Sb2 = $S.ToString('0.########', $Inv); $Wb = $W.ToString('0.########', $Inv)
$Nb = $N.ToString('0.########', $Inv); $Eb = $E.ToString('0.########', $Inv)
foreach ($Kv in $Kinds) {
	$Kind = $Kv[0]; $Filter = $Kv[1]
	$Q = "[out:json][timeout:180];($Filter($Sb2,$Wb,$Nb,$Eb););out center;"
	$Body = 'data=' + [Uri]::EscapeDataString($Q)
	$O = $null
	# Les miroirs saturent aux heures de pointe (504) : patience longue, et une
	# categorie irrecuperable est SAUTEE (rapportee), pas fatale.
	for ($Try = 0; $Try -lt 9; $Try++) {
		$Mirror = $Mirrors[$Try % $Mirrors.Count]
		try {
			# Decodage UTF-8 EXPLICITE (piege mojibake PS 5.1).
			$Resp = Invoke-WebRequest -Uri $Mirror -Method Post -Body $Body `
				-ContentType 'application/x-www-form-urlencoded' `
				-UserAgent 'CityLab-DroneFPV/1.0 (contact: mourradmohsen@gmail.com)' -TimeoutSec 200
			$O = [System.Text.Encoding]::UTF8.GetString($Resp.RawContentStream.ToArray()) | ConvertFrom-Json
			break
		} catch {
			Write-Host "  $Kind : echec sur $Mirror ($($_.Exception.Message)), retry 45 s"
			Start-Sleep 45
		}
	}
	if (-not $O) {
		Write-Host "  $Kind : SAUTE apres 9 tentatives (a rejouer plus tard)"
		$Counts[$Kind] = -1
		continue
	}
	$NbK = 0
	foreach ($El in $O.elements) {
		if (-not $Seen.Add("$($El.type)/$($El.id)")) { continue }
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
		$NbTot++; $NbK++
	}
	$Counts[$Kind] = $NbK
	Write-Host "  $Kind : $NbK"
	Start-Sleep 3
}

$Out = '{"source":"OSM Overpass (ODbL) - mobilier urbain, couverture INDICATIVE","items":[' + $Sb.ToString() + ']}'
$Path = Join-Path $PSScriptRoot 'mobilier.json'
[System.IO.File]::WriteAllText($Path, $Out, (New-Object System.Text.UTF8Encoding $false))
$Skipped = @($Counts.Keys | Where-Object { $Counts[$_] -eq -1 })
Write-Host "FIN : $NbTot elements -> $Path ($([Math]::Round((Get-Item $Path).Length/1KB,0)) Ko)"
if ($Skipped.Count -gt 0) { Write-Host "CATEGORIES SAUTEES (a rejouer) : $($Skipped -join ', ')" }