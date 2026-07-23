# Recupere les donnees reelles du quartier Capitole (Toulouse) et produit un JSON local
# en metres (x = est, y = nord, origine au centre du quartier) :
#  - batiments : BD TOPO IGN (WFS Geoplateforme) -> emprise polygonale + hauteur + usage
#  - routes + arbres : OpenStreetMap (Overpass, miroir Kumi)
$ErrorActionPreference = 'Stop'

# Emprise ~600 x 600 m centree sur le Capitole
$Lat0 = 43.6045; $Lon0 = 1.4442
$DLat = 0.0027;  $DLon = 0.00373
$S = $Lat0 - $DLat; $N = $Lat0 + $DLat; $W = $Lon0 - $DLon; $E = $Lon0 + $DLon
$MPerLat = 110540.0
$MPerLon = 111320.0 * [Math]::Cos($Lat0 * [Math]::PI / 180)

function ToLocal([double]$Lat, [double]$Lon) {
	return @([Math]::Round(($Lon - $Lon0) * $MPerLon, 2), [Math]::Round(($Lat - $Lat0) * $MPerLat, 2))
}

# ---------- Batiments : BD TOPO via WFS (pagine) ----------
Write-Host 'BD TOPO : telechargement des batiments...'
$Buildings = New-Object System.Collections.Generic.List[object]
$Start = 0
do {
	$Url = "https://data.geopf.fr/wfs/ows?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=BDTOPO_V3:batiment&SRSNAME=EPSG:4326&BBOX=$W,$S,$E,$N,EPSG:4326&OUTPUTFORMAT=application/json&COUNT=1000&STARTINDEX=$Start"
	$R = Invoke-RestMethod -Uri $Url -TimeoutSec 120 -UserAgent 'CityLab-DroneFPV/1.0'
	foreach ($F in $R.features) {
		if ($F.properties.etat_de_l_objet -and $F.properties.etat_de_l_objet -ne 'En service') { continue }
		# MultiPolygon -> anneau exterieur du premier polygone
		$Ring = $F.geometry.coordinates[0][0]
		if (-not $Ring -or $Ring.Count -lt 4) { continue }
		$Pts = New-Object System.Collections.Generic.List[object]
		$Prev = $null
		foreach ($C in $Ring) {
			$P = ToLocal $C[1] $C[0]
			if ($Prev -and ([Math]::Abs($P[0]-$Prev[0]) + [Math]::Abs($P[1]-$Prev[1])) -lt 0.8) { continue }
			$Pts.Add($P); $Prev = $P
		}
		# fermer l'anneau = retirer le doublon final si present
		if ($Pts.Count -gt 1) {
			$A = $Pts[0]; $B = $Pts[$Pts.Count-1]
			if (([Math]::Abs($A[0]-$B[0]) + [Math]::Abs($A[1]-$B[1])) -lt 0.8) { $Pts.RemoveAt($Pts.Count-1) }
		}
		if ($Pts.Count -lt 3 -or $Pts.Count -gt 80) { continue }
		$H = $F.properties.hauteur
		if (-not $H -or $H -le 0) {
			$Et = $F.properties.nombre_d_etages
			if ($Et -and $Et -gt 0) { $H = 3.0 * $Et + 1.0 } else { $H = 9.0 }
		}
		$U = switch -Wildcard ("$($F.properties.usage_1)") {
			'R*sidentiel' { 'res' }
			'Commercial*' { 'com' }
			'Industriel*' { 'ind' }
			default { 'oth' }
		}
		$Buildings.Add(@{ pts = $Pts.ToArray(); h = [Math]::Round([double]$H, 1); u = $U })
	}
	$Got = $R.features.Count
	$Start += 1000
	Write-Host "  page : $Got features (total lu $($Buildings.Count))"
} while ($Got -eq 1000)
Write-Host "Batiments retenus : $($Buildings.Count)"

# ---------- Routes + arbres : Overpass ----------
Write-Host 'Overpass : routes et arbres...'
$Q = @"
[out:json][timeout:120];
(way["highway"]($S,$W,$N,$E););out geom;
node["natural"="tree"]($S,$W,$N,$E);out;
"@
$Body = 'data=' + [Uri]::EscapeDataString($Q)
$O = Invoke-RestMethod -Uri 'https://overpass.kumi.systems/api/interpreter' -Method Post -Body $Body -ContentType 'application/x-www-form-urlencoded' -UserAgent 'CityLab-DroneFPV/1.0 (contact: mourradmohsen@gmail.com)' -TimeoutSec 300

$WidthByType = @{ motorway=11; trunk=11; primary=10; secondary=9; tertiary=8; unclassified=6; residential=6; living_street=5; pedestrian=5; service=4; footway=2.5; path=2.5; cycleway=2.5; track=3 }
$Roads = New-Object System.Collections.Generic.List[object]
$Trees = New-Object System.Collections.Generic.List[object]
foreach ($El in $O.elements) {
	if ($El.type -eq 'way' -and $El.geometry) {
		$T = "$($El.tags.highway)"
		if ($T -in @('steps','corridor','elevator','proposed','construction')) { continue }
		$W2 = $WidthByType[$T]; if (-not $W2) { $W2 = 5 }
		$Pts = @(); foreach ($G in $El.geometry) { $Pts += , (ToLocal $G.lat $G.lon) }
		if ($Pts.Count -ge 2) { $Roads.Add(@{ pts = $Pts; t = $T; w = $W2 }) }
	} elseif ($El.type -eq 'node') {
		$Trees.Add((ToLocal $El.lat $El.lon))
	}
}
Write-Host "Routes : $($Roads.Count) / Arbres : $($Trees.Count)"

# ---------- Sortie ----------
$Out = @{ origin = @{ lat = $Lat0; lon = $Lon0 }; sizeM = @{ x = [Math]::Round(2*$DLon*$MPerLon,0); y = [Math]::Round(2*$DLat*$MPerLat,0) }
	buildings = $Buildings; roads = $Roads; trees = $Trees }
$Dir = 'C:\Users\User\Documents\Unreal Projects\CityLab\SourceData'
New-Item -ItemType Directory -Force $Dir | Out-Null
$Path = Join-Path $Dir 'capitole.json'
$Out | ConvertTo-Json -Depth 8 -Compress | Out-File -FilePath $Path -Encoding utf8
"JSON ecrit : $Path ($([Math]::Round((Get-Item $Path).Length/1MB,1)) Mo)"
