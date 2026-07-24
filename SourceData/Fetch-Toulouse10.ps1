# Recupere les donnees reelles de Toulouse sur 10 x 10 km (meme origine que capitole.json :
# place du Capitole) et produit toulouse10.json, schema identique a capitole.json :
#  - batiments : BD TOPO IGN (WFS Geoplateforme, pagine ~132 pages)
#  - routes + arbres : OpenStreetMap (Overpass, en tuiles 2 x 2 km, dedup par id)
# JSON ecrit au StringBuilder (ConvertTo-Json est trop lent a cette echelle).
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
# NORD = -Y : Unreal est main GAUCHE — garder nord=+Y refletait toute la ville en
# miroir (chiralite inversee, constatee en vol). Convention Cesium/geo standard.
function ToLocalY([double]$Lat) { return [Math]::Round(($Lat0 - $Lat) * $MPerLat, 2) }

# ---------- Batiments : BD TOPO via WFS (pagine) ----------
Write-Host "BD TOPO : telechargement des batiments ($([Math]::Round(2*$HalfM/1000,1)) km de cote)..."
$SbB = New-Object System.Text.StringBuilder
$NbB = 0
$Start = 0
do {
	$Url = "https://data.geopf.fr/wfs/ows?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=BDTOPO_V3:batiment&SRSNAME=EPSG:4326&BBOX=$W,$S,$E,$N,EPSG:4326&OUTPUTFORMAT=application/json&COUNT=1000&STARTINDEX=$Start"
	$R = $null
	for ($Try = 1; $Try -le 6; $Try++) {
		try { $R = Invoke-RestMethod -Uri $Url -TimeoutSec 180 -UserAgent 'CityLab-DroneFPV/1.0'; break }
		catch {
			if ($Try -eq 6) { throw }
			Write-Host "  WFS page $Start : echec ($Try) [$($_.Exception.Message)], retry dans 5 s"
			Start-Sleep 5
		}
	}
	foreach ($Feat in $R.features) {
		if ($Feat.properties.etat_de_l_objet -and $Feat.properties.etat_de_l_objet -ne 'En service') { continue }
		# MultiPolygon -> anneau exterieur du premier polygone
		$Ring = $Feat.geometry.coordinates[0][0]
		if (-not $Ring -or $Ring.Count -lt 4) { continue }
		$Pts = New-Object System.Collections.Generic.List[object]
		$Prev = $null
		foreach ($C in $Ring) {
			$P = @((ToLocalX $C[0]), (ToLocalY $C[1]))
			if ($Prev -and ([Math]::Abs($P[0]-$Prev[0]) + [Math]::Abs($P[1]-$Prev[1])) -lt 0.8) { continue }
			$Pts.Add($P); $Prev = $P
		}
		if ($Pts.Count -gt 1) {
			$A = $Pts[0]; $B = $Pts[$Pts.Count-1]
			if (([Math]::Abs($A[0]-$B[0]) + [Math]::Abs($A[1]-$B[1])) -lt 0.8) { $Pts.RemoveAt($Pts.Count-1) }
		}
		if ($Pts.Count -lt 3 -or $Pts.Count -gt 80) { continue }
		$H = $Feat.properties.hauteur
		if (-not $H -or $H -le 0) {
			$Et = $Feat.properties.nombre_d_etages
			if ($Et -and $Et -gt 0) { $H = 3.0 * $Et + 1.0 } else { $H = 9.0 }
		}
		$U = switch -Wildcard ("$($Feat.properties.usage_1)") {
			'R*sidentiel' { 'res' }
			'Commercial*' { 'com' }
			'Industriel*' { 'ind' }
			default { 'oth' }
		}
		if ($NbB -gt 0) { [void]$SbB.Append(',') }
		[void]$SbB.Append('{"pts":[')
		for ($i = 0; $i -lt $Pts.Count; $i++) {
			if ($i -gt 0) { [void]$SbB.Append(',') }
			[void]$SbB.Append('[').Append((F $Pts[$i][0])).Append(',').Append((F $Pts[$i][1])).Append(']')
		}
		[void]$SbB.Append('],"h":').Append((F ([Math]::Round([double]$H, 1)))).Append(',"u":"').Append($U).Append('"}')
		$NbB++
	}
	$Got = $R.features.Count
	$Start += 1000
	if (($Start % 10000) -eq 0) { Write-Host "  WFS : $Start lus, $NbB retenus" }
	# La Geoplateforme jette les requetes enchainees sans pause (throttling).
	Start-Sleep -Milliseconds 1200
} while ($Got -eq 1000)
Write-Host "Batiments retenus : $NbB"

# ---------- Routes + arbres : Overpass en tuiles 2 x 2 km ----------
Write-Host 'Overpass : routes et arbres en 25 tuiles...'
$Mirrors = @('https://overpass.openstreetmap.fr/api/interpreter',
	'https://overpass.kumi.systems/api/interpreter',
	'https://overpass-api.de/api/interpreter')
$WidthByType = @{ motorway=11; trunk=11; primary=10; secondary=9; tertiary=8; unclassified=6; residential=6; living_street=5; pedestrian=5; service=4; footway=2.5; path=2.5; cycleway=2.5; track=3 }
$SeenWays = New-Object 'System.Collections.Generic.HashSet[long]'
$SeenTrees = New-Object 'System.Collections.Generic.HashSet[long]'
$SbR = New-Object System.Text.StringBuilder
$SbT = New-Object System.Text.StringBuilder
$NbR = 0; $NbT = 0
$Tiles = 5
for ($Ty = 0; $Ty -lt $Tiles; $Ty++) {
	for ($Tx = 0; $Tx -lt $Tiles; $Tx++) {
		$TS = $S + ($N - $S) * $Ty / $Tiles; $TN = $S + ($N - $S) * ($Ty + 1) / $Tiles
		$TW = $W + ($E - $W) * $Tx / $Tiles; $TE = $W + ($E - $W) * ($Tx + 1) / $Tiles
		$Q = @"
[out:json][timeout:120];
(way["highway"]["tunnel"!="yes"]["covered"!="yes"]($TS,$TW,$TN,$TE););out geom;
node["natural"="tree"]($TS,$TW,$TN,$TE);out;
"@
		$Body = 'data=' + [Uri]::EscapeDataString($Q)
		$O = $null
		for ($Try = 0; $Try -lt 6; $Try++) {
			$Mirror = $Mirrors[$Try % $Mirrors.Count]
			try {
				$O = Invoke-RestMethod -Uri $Mirror -Method Post -Body $Body -ContentType 'application/x-www-form-urlencoded' -UserAgent 'CityLab-DroneFPV/1.0 (contact: mourradmohsen@gmail.com)' -TimeoutSec 240
				break
			} catch {
				Write-Host "  tuile ($Tx,$Ty) : echec sur $Mirror, retry dans 20 s"
				Start-Sleep 20
			}
		}
		if (-not $O) { throw "Overpass : tuile ($Tx,$Ty) irrecuperable apres 6 essais." }
		$TileR = 0; $TileT = 0
		foreach ($El in $O.elements) {
			if ($El.type -eq 'way' -and $El.geometry) {
				if (-not $SeenWays.Add([long]$El.id)) { continue }
				$T = "$($El.tags.highway)"
				if ($T -in @('steps','corridor','elevator','proposed','construction')) { continue }
				$W2 = $WidthByType[$T]; if (-not $W2) { $W2 = 5 }
				$NPts = 0
				$SbP = New-Object System.Text.StringBuilder
				foreach ($G in $El.geometry) {
					if ($NPts -gt 0) { [void]$SbP.Append(',') }
					[void]$SbP.Append('[').Append((F (ToLocalX $G.lon))).Append(',').Append((F (ToLocalY $G.lat))).Append(']')
					$NPts++
				}
				if ($NPts -ge 2) {
					if ($NbR -gt 0) { [void]$SbR.Append(',') }
					[void]$SbR.Append('{"pts":[').Append($SbP.ToString()).Append('],"t":"').Append($T).Append('","w":').Append((F $W2)).Append('}')
					$NbR++; $TileR++
				}
			} elseif ($El.type -eq 'node') {
				if (-not $SeenTrees.Add([long]$El.id)) { continue }
				if ($NbT -gt 0) { [void]$SbT.Append(',') }
				[void]$SbT.Append('[').Append((F (ToLocalX $El.lon))).Append(',').Append((F (ToLocalY $El.lat))).Append(']')
				$NbT++; $TileT++
			}
		}
		Write-Host "  tuile ($Tx,$Ty) : +$TileR routes, +$TileT arbres (total $NbR / $NbT)"
		Start-Sleep 2
	}
}
Write-Host "Routes : $NbR / Arbres : $NbT"

# ---------- Sortie ----------
$Sb = New-Object System.Text.StringBuilder
[void]$Sb.Append('{"origin":{"lat":').Append($Lat0.ToString($Inv)).Append(',"lon":').Append($Lon0.ToString($Inv)).Append('},')
[void]$Sb.Append('"sizeM":{"x":').Append((F (2*$HalfM))).Append(',"y":').Append((F (2*$HalfM))).Append('},')
[void]$Sb.Append('"buildings":[').Append($SbB.ToString()).Append('],')
[void]$Sb.Append('"roads":[').Append($SbR.ToString()).Append('],')
[void]$Sb.Append('"trees":[').Append($SbT.ToString()).Append(']}')
$Path = Join-Path $PSScriptRoot 'toulouse10.json'
[System.IO.File]::WriteAllText($Path, $Sb.ToString(), (New-Object System.Text.UTF8Encoding $false))
"JSON ecrit : $Path ($([Math]::Round((Get-Item $Path).Length/1MB,1)) Mo) - $NbB batiments, $NbR routes, $NbT arbres"
