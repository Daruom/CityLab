# Fetch-GF-Poi.ps1 -- GRAND FETCH theme 8 : points et zones d'interet.
# (a) BDTOPO_V3:zone_d_activite_ou_d_interet : polygones + categorie/nature/toponyme bruts.
# (b) OSM Overpass : nodes+ways nommes avec amenity|tourism|historic -> centroide
#     (out center) + nom + cle/valeur. 4 quadrants, dedup type/id.
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'poi'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'poi.json'
$Bd = GFNewBounds
GFLog $Theme 'DEBUT (zone_d_activite_ou_d_interet + POI OSM)'

# ---------- (a) ZAI BD TOPO ----------
$SbZ = New-Object System.Text.StringBuilder
$KeysZ = $null; $FillZ = @{}; $NbZ = 0; $NbZSkip = 0
$TemoinZ = $null; $TemoinZAccent = $null
foreach ($Feat in (GFWfsAll 'BDTOPO_V3:zone_d_activite_ou_d_interet' $B $Theme)) {
	$Props = $Feat.properties
	if ($null -eq $KeysZ) {
		$KeysZ = GFScalarKeys $Props
		GFLog $Theme ('attributs ZAI : ' + ($KeysZ -join ', '))
	}
	$Rings = GFExteriorRings $Feat.geometry
	$SbR = New-Object System.Text.StringBuilder
	$NbR = 0
	foreach ($Ring in $Rings) {
		$Pts = GFThinRing $Ring 2.0 400
		if ($Pts.Count -lt 3) { continue }
		if ($NbR -gt 0) { [void]$SbR.Append(',') }
		GFAppendPts $SbR $Pts
		GFBoundsPts $Bd $Pts
		$NbR++
	}
	if ($NbR -lt 1) {
		$G = $Feat.geometry
		if ($G -and $G.type -eq 'Point') {
			$X = ToLocalX $G.coordinates[0]; $Y = ToLocalY $G.coordinates[1]
			if ($NbZ -gt 0) { [void]$SbZ.Append(',') }
			[void]$SbZ.Append('{"x":').Append((F $X)).Append(',"y":').Append((F $Y))
			GFAppendPropsCount $SbZ $Props $KeysZ $FillZ
			[void]$SbZ.Append('}')
			GFBoundsPts $Bd @(, @($X, $Y))
			$NbZ++
		} else { $NbZSkip++ }
		continue
	}
	if ($NbZ -gt 0) { [void]$SbZ.Append(',') }
	[void]$SbZ.Append('{"rings":[').Append($SbR.ToString()).Append(']')
	GFAppendPropsCount $SbZ $Props $KeysZ $FillZ
	[void]$SbZ.Append('}')
	$NbZ++
	$Topo = "$($Props.toponyme)"
	if ($Topo -ne '') {
		if (-not $TemoinZ) { $TemoinZ = "ZAI $($Props.categorie) / $($Props.nature) : $Topo" }
		if (-not $TemoinZAccent -and $Topo -match '[^\x00-\x7f]') { $TemoinZAccent = "ZAI $($Props.categorie) / $($Props.nature) : $Topo" }
	}
}
GFLog $Theme "ZAI : $NbZ ($NbZSkip sans geometrie exploitable)"

# ---------- (b) POI OSM ----------
$Seen = New-Object 'System.Collections.Generic.HashSet[string]'
$SbO = New-Object System.Text.StringBuilder
$NbO = 0
$CountByKey = @{}
$TemoinO = $null; $TemoinOAccent = $null
for ($Ty = 0; $Ty -lt 2; $Ty++) {
	for ($Tx = 0; $Tx -lt 2; $Tx++) {
		$TS = $B.S + ($B.N - $B.S) * $Ty / 2; $TN = $B.S + ($B.N - $B.S) * ($Ty + 1) / 2
		$TW = $B.W + ($B.E - $B.W) * $Tx / 2; $TE = $B.W + ($B.E - $B.W) * ($Tx + 1) / 2
		$Q = @"
[out:json][timeout:180];
(
  node["name"]["amenity"]($TS,$TW,$TN,$TE);
  node["name"]["tourism"]($TS,$TW,$TN,$TE);
  node["name"]["historic"]($TS,$TW,$TN,$TE);
  way["name"]["amenity"]($TS,$TW,$TN,$TE);
  way["name"]["tourism"]($TS,$TW,$TN,$TE);
  way["name"]["historic"]($TS,$TW,$TN,$TE);
);
out center;
"@
		$O = GFOverpass $Q $Theme "POI quadrant ($Tx,$Ty)"
		foreach ($El in $O.elements) {
			if (-not $Seen.Add("$($El.type)/$($El.id)")) { continue }
			$Lat = $null; $Lon = $null
			if ($El.type -eq 'node') { $Lat = $El.lat; $Lon = $El.lon }
			elseif ($El.center) { $Lat = $El.center.lat; $Lon = $El.center.lon }
			if ($null -eq $Lat) { continue }
			$Name = "$($El.tags.name)"
			if ($Name -eq '') { continue }
			$K = $null; $V = $null
			if ($El.tags.amenity) { $K = 'amenity'; $V = "$($El.tags.amenity)" }
			elseif ($El.tags.tourism) { $K = 'tourism'; $V = "$($El.tags.tourism)" }
			elseif ($El.tags.historic) { $K = 'historic'; $V = "$($El.tags.historic)" }
			if ($null -eq $K) { continue }
			$X = ToLocalX $Lon; $Y = ToLocalY $Lat
			if ($NbO -gt 0) { [void]$SbO.Append(',') }
			[void]$SbO.Append('{"x":').Append((F $X)).Append(',"y":').Append((F $Y))
			[void]$SbO.Append(',"name":"').Append((GFEsc $Name)).Append('","k":"').Append($K).Append('","v":"').Append((GFEsc $V)).Append('"}')
			GFBoundsPts $Bd @(, @($X, $Y))
			if ($CountByKey.ContainsKey($K)) { $CountByKey[$K] = $CountByKey[$K] + 1 } else { $CountByKey[$K] = 1 }
			if (-not $TemoinO -and $K -eq 'historic') { $TemoinO = "$K=$V : $Name X=$(F $X) Y=$(F $Y)" }
			if (-not $TemoinOAccent -and $Name -match '[^\x00-\x7f]') { $TemoinOAccent = "$K=$V : $Name X=$(F $X) Y=$(F $Y)" }
			$NbO++
		}
		GFLog $Theme "POI quadrant ($Tx,$Ty) : cumul $NbO"
		Start-Sleep 2
	}
}

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"zai":[')
[void]$Out.Append($SbZ.ToString()).Append('],"osm":[').Append($SbO.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$Cles = New-Object System.Collections.Generic.List[string]
foreach ($K in ($CountByKey.Keys | Sort-Object)) { $Cles.Add("$K=$($CountByKey[$K])") }
$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## poi.json -- zones et points d interet')
$L.Add("- Sources : WFS Geoplateforme BDTOPO_V3:zone_d_activite_ou_d_interet (Licence Ouverte 2.0 IGN) ; OSM nodes+ways nommes amenity|tourism|historic, centroides via out center (ODbL 1.0). Fetch du $(Get-Date -Format 'yyyy-MM-dd').")
$L.Add("- Objets : $NbZ ZAI ($NbZSkip sans geometrie) ; $NbO POI OSM (" + ($Cles -join ', ') + ').')
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage ZAI :')
if ($KeysZ) { $L.Add((GFFillText $KeysZ $FillZ $NbZ)) }
$L.Add('- POI OSM : name 100 % (filtre requete) ; cle unique par priorite amenity > tourism > historic.')
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- Temoins :')
if ($TemoinZ) { $L.Add("  - $TemoinZ") }
if ($TemoinO) { $L.Add("  - $TemoinO") }
if ($TemoinZAccent) { $L.Add("  - (accent) $TemoinZAccent") }
if ($TemoinOAccent) { $L.Add("  - (accent) $TemoinOAccent") }
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $NbZ ZAI + $NbO POI OSM, $SizeMo Mo"
