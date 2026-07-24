# Recupere les surfaces de reperage de la ville 10 x 10 km -> toulouse10_surfaces.json
#  - eau (Garonne, Canal du Midi, lacs) : BD TOPO surface_hydrographique (polygones propres)
#  - bois/forets : BD TOPO zone_de_vegetation (nature Bois/Foret/Peupleraie)
#  - parcs/pelouses : OSM leisure=park|garden + landuse herbeux + natural=wood (ways fermes)
#  - voies ferrees : OSM railway=rail|tram hors tunnel (polylignes)
# Anneaux amincis (>= 4 m entre points) et plafonnes a 600 points (ear-clipping O(n^3)).
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
# NORD = -Y (chiralite Unreal main gauche — cf. Fetch-Toulouse10.ps1).
function ToLocalY([double]$Lat) { return [Math]::Round(($Lat0 - $Lat) * $MPerLat, 2) }

# Amincit un anneau : points a >= 8 m d'ecart, plafond 400 points (une ondulation de
# bord de 8 m est invisible a toute distance de vol ; ~/2 sur les tris de surfaces).
function ThinRing($Ring) {
	$Pts = New-Object System.Collections.Generic.List[object]
	$Prev = $null
	foreach ($C in $Ring) {
		$P = @((ToLocalX $C[0]), (ToLocalY $C[1]))
		if ($Prev -and ([Math]::Abs($P[0]-$Prev[0]) + [Math]::Abs($P[1]-$Prev[1])) -lt 8.0) { continue }
		$Pts.Add($P); $Prev = $P
	}
	if ($Pts.Count -gt 1) {
		$A = $Pts[0]; $B = $Pts[$Pts.Count-1]
		if (([Math]::Abs($A[0]-$B[0]) + [Math]::Abs($A[1]-$B[1])) -lt 8.0) { $Pts.RemoveAt($Pts.Count-1) }
	}
	if ($Pts.Count -gt 400) {
		$Step = [Math]::Ceiling($Pts.Count / 400.0)
		$Thin = New-Object System.Collections.Generic.List[object]
		for ($i = 0; $i -lt $Pts.Count; $i += $Step) { $Thin.Add($Pts[$i]) }
		$Pts = $Thin
	}
	return $Pts
}

function AppendPoly([System.Text.StringBuilder]$Sb, [ref]$Count, $Pts, [string]$Kind) {
	if ($Pts.Count -lt 3) { return }
	if ($Count.Value -gt 0) { [void]$Sb.Append(',') }
	[void]$Sb.Append('{')
	if ($Kind) { [void]$Sb.Append('"k":"').Append($Kind).Append('",') }
	[void]$Sb.Append('"pts":[')
	for ($i = 0; $i -lt $Pts.Count; $i++) {
		if ($i -gt 0) { [void]$Sb.Append(',') }
		[void]$Sb.Append('[').Append((F $Pts[$i][0])).Append(',').Append((F $Pts[$i][1])).Append(']')
	}
	[void]$Sb.Append(']}')
	$Count.Value++
}

function FetchWfs([string]$Layer) {
	$Out = New-Object System.Collections.Generic.List[object]
	$Start = 0
	do {
		$Url = "https://data.geopf.fr/wfs/ows?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=BDTOPO_V3:$Layer&SRSNAME=EPSG:4326&BBOX=$W,$S,$E,$N,EPSG:4326&OUTPUTFORMAT=application/json&COUNT=1000&STARTINDEX=$Start"
		$R = $null
		for ($Try = 1; $Try -le 6; $Try++) {
			try { $R = Invoke-RestMethod -Uri $Url -TimeoutSec 180 -UserAgent 'CityLab-DroneFPV/1.0'; break }
			catch { if ($Try -eq 6) { throw }; Write-Host "  WFS $Layer $Start : echec, retry 5 s"; Start-Sleep 5 }
		}
		foreach ($Feat in $R.features) { $Out.Add($Feat) }
		$Got = $R.features.Count
		$Start += 1000
		Start-Sleep -Milliseconds 1200
	} while ($Got -eq 1000)
	return ,$Out
}

# ---------- Eau : BD TOPO ----------
Write-Host 'BD TOPO : surfaces d''eau...'
$SbW = New-Object System.Text.StringBuilder; $NbW = 0
foreach ($Feat in (FetchWfs 'surface_hydrographique')) {
	$Ring = $Feat.geometry.coordinates[0][0]
	if (-not $Ring -or $Ring.Count -lt 4) { continue }
	AppendPoly $SbW ([ref]$NbW) (ThinRing $Ring) ''
}
Write-Host "Eau : $NbW polygones"

# ---------- Bois : BD TOPO ----------
Write-Host 'BD TOPO : zones de vegetation (bois/forets)...'
$SbG = New-Object System.Text.StringBuilder; $NbG = 0
foreach ($Feat in (FetchWfs 'zone_de_vegetation')) {
	if ("$($Feat.properties.nature)" -notmatch 'Bois|For|Peupleraie') { continue }
	$Ring = $Feat.geometry.coordinates[0][0]
	if (-not $Ring -or $Ring.Count -lt 4) { continue }
	AppendPoly $SbG ([ref]$NbG) (ThinRing $Ring) 'forest'
}
Write-Host "Bois : $NbG polygones"

# ---------- Parcs + rails : OSM en 4 quadrants ----------
Write-Host 'Overpass : parcs, pelouses, rails...'
$Mirrors = @('https://overpass.openstreetmap.fr/api/interpreter',
	'https://overpass.kumi.systems/api/interpreter',
	'https://overpass-api.de/api/interpreter')
$Seen = New-Object 'System.Collections.Generic.HashSet[long]'
$SbR = New-Object System.Text.StringBuilder; $NbR = 0
for ($Ty = 0; $Ty -lt 2; $Ty++) {
	for ($Tx = 0; $Tx -lt 2; $Tx++) {
		$TS = $S + ($N - $S) * $Ty / 2; $TN = $S + ($N - $S) * ($Ty + 1) / 2
		$TW = $W + ($E - $W) * $Tx / 2; $TE = $W + ($E - $W) * ($Tx + 1) / 2
		$Q = @"
[out:json][timeout:120];
(
  way["leisure"~"^(park|garden)$"]($TS,$TW,$TN,$TE);
  way["landuse"~"^(grass|meadow|village_green|recreation_ground)$"]($TS,$TW,$TN,$TE);
  way["natural"="wood"]($TS,$TW,$TN,$TE);
  way["railway"~"^(rail|tram)$"]["tunnel"!="yes"]($TS,$TW,$TN,$TE);
);
out geom;
"@
		$Body = 'data=' + [Uri]::EscapeDataString($Q)
		$O = $null
		for ($Try = 0; $Try -lt 6; $Try++) {
			$Mirror = $Mirrors[$Try % $Mirrors.Count]
			try {
				$O = Invoke-RestMethod -Uri $Mirror -Method Post -Body $Body -ContentType 'application/x-www-form-urlencoded' -UserAgent 'CityLab-DroneFPV/1.0 (contact: mourradmohsen@gmail.com)' -TimeoutSec 240
				break
			} catch { Write-Host "  quadrant ($Tx,$Ty) : echec sur $Mirror, retry 20 s"; Start-Sleep 20 }
		}
		if (-not $O) { throw "Overpass : quadrant ($Tx,$Ty) irrecuperable." }
		foreach ($El in $O.elements) {
			if ($El.type -ne 'way' -or -not $El.geometry) { continue }
			if (-not $Seen.Add([long]$El.id)) { continue }
			if ($El.tags.railway) {
				# Polyligne rail : memes points, pas de fermeture requise.
				$Pts = New-Object System.Collections.Generic.List[object]
				$Prev = $null
				foreach ($G in $El.geometry) {
					$P = @((ToLocalX $G.lon), (ToLocalY $G.lat))
					if ($Prev -and ([Math]::Abs($P[0]-$Prev[0]) + [Math]::Abs($P[1]-$Prev[1])) -lt 2.0) { continue }
					$Pts.Add($P); $Prev = $P
				}
				if ($Pts.Count -ge 2) {
					if ($NbR -gt 0) { [void]$SbR.Append(',') }
					[void]$SbR.Append('{"pts":[')
					for ($i = 0; $i -lt $Pts.Count; $i++) {
						if ($i -gt 0) { [void]$SbR.Append(',') }
						[void]$SbR.Append('[').Append((F $Pts[$i][0])).Append(',').Append((F $Pts[$i][1])).Append(']')
					}
					[void]$SbR.Append(']}')
					$NbR++
				}
			} else {
				# Polygone vert : way ferme uniquement.
				$First = $El.geometry[0]; $Last = $El.geometry[$El.geometry.Count-1]
				if ([Math]::Abs($First.lat-$Last.lat) -gt 1e-7 -or [Math]::Abs($First.lon-$Last.lon) -gt 1e-7) { continue }
				$Ring = @(); foreach ($G in $El.geometry) { $Ring += , @($G.lon, $G.lat) }
				$Kind = if ($El.tags.natural -eq 'wood') { 'forest' } else { 'park' }
				AppendPoly $SbG ([ref]$NbG) (ThinRing $Ring) $Kind
			}
		}
		Write-Host "  quadrant ($Tx,$Ty) : cumul vert=$NbG rails=$NbR"
		Start-Sleep 2
	}
}

# ---------- Sortie ----------
$Sb = New-Object System.Text.StringBuilder
[void]$Sb.Append('{"water":[').Append($SbW.ToString()).Append('],')
[void]$Sb.Append('"green":[').Append($SbG.ToString()).Append('],')
[void]$Sb.Append('"rails":[').Append($SbR.ToString()).Append(']}')
$Path = Join-Path $PSScriptRoot 'toulouse10_surfaces.json'
[System.IO.File]::WriteAllText($Path, $Sb.ToString(), (New-Object System.Text.UTF8Encoding $false))
"JSON ecrit : $Path ($([Math]::Round((Get-Item $Path).Length/1MB,2)) Mo) - eau=$NbW vert=$NbG rails=$NbR"
