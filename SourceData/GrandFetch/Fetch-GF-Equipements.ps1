# Fetch-GF-Equipements.ps1 -- GRAND FETCH theme 9 : equipements.
# BDTOPO_V3:terrain_de_sport + BDTOPO_V3:cimetiere (polygones + attributs bruts),
# parkings : BDTOPO_V3:equipement_de_transport filtre nature parking (pas de couche
# parking dediee) + OSM way[amenity=parking] (polygones fermes, 4 quadrants).
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'equipements'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'equipements.json'
$Bd = GFNewBounds
GFLog $Theme 'DEBUT (terrain_de_sport + cimetiere + parkings BDTOPO/OSM)'

# Emission generique de surfaces WFS -> {"rings":[...], props}
function EmitWfsSurf([string]$Layer, [string]$Filtre, [System.Text.StringBuilder]$Sb, $Fill, [ref]$KeysRef, [string]$Theme2) {
	$Nb = 0
	foreach ($Feat in (GFWfsAll $Layer (GFBbox 5000.0) $Theme2)) {
		$Props = $Feat.properties
		if ($Filtre -and "$($Props.nature)" -notmatch $Filtre) { continue }
		if ($null -eq $KeysRef.Value) { $KeysRef.Value = GFScalarKeys $Props }
		$Rings = GFExteriorRings $Feat.geometry
		$SbR = New-Object System.Text.StringBuilder
		$NbR = 0
		foreach ($Ring in $Rings) {
			$Pts = GFThinRing $Ring 2.0 400
			if ($Pts.Count -lt 3) { continue }
			if ($NbR -gt 0) { [void]$SbR.Append(',') }
			GFAppendPts $SbR $Pts
			GFBoundsPts $script:Bd $Pts
			$NbR++
		}
		if ($NbR -lt 1) {
			# Certains equipements sont ponctuels : garder le point.
			$G = $Feat.geometry
			if ($G -and $G.type -eq 'Point') {
				$X = ToLocalX $G.coordinates[0]; $Y = ToLocalY $G.coordinates[1]
				if ($Nb -gt 0) { [void]$Sb.Append(',') }
				[void]$Sb.Append('{"x":').Append((F $X)).Append(',"y":').Append((F $Y))
				GFAppendPropsCount $Sb $Props $KeysRef.Value $Fill
				[void]$Sb.Append('}')
				GFBoundsPts $script:Bd @(, @($X, $Y))
				$Nb++
			}
			continue
		}
		if ($Nb -gt 0) { [void]$Sb.Append(',') }
		[void]$Sb.Append('{"rings":[').Append($SbR.ToString()).Append(']')
		GFAppendPropsCount $Sb $Props $KeysRef.Value $Fill
		[void]$Sb.Append('}')
		$Nb++
	}
	return $Nb
}

$SbSport = New-Object System.Text.StringBuilder
$KeysSport = $null; $FillSport = @{}
$NbSport = EmitWfsSurf 'BDTOPO_V3:terrain_de_sport' '' $SbSport $FillSport ([ref]$KeysSport) $Theme
GFLog $Theme "terrains de sport : $NbSport"

$SbCim = New-Object System.Text.StringBuilder
$KeysCim = $null; $FillCim = @{}
$NbCim = EmitWfsSurf 'BDTOPO_V3:cimetiere' '' $SbCim $FillCim ([ref]$KeysCim) $Theme
GFLog $Theme "cimetieres : $NbCim"

$SbPkB = New-Object System.Text.StringBuilder
$KeysPkB = $null; $FillPkB = @{}
$NbPkB = EmitWfsSurf 'BDTOPO_V3:equipement_de_transport' 'parking' $SbPkB $FillPkB ([ref]$KeysPkB) $Theme
GFLog $Theme "parkings BD TOPO (equipement_de_transport nature~parking) : $NbPkB"

# ---------- Parkings OSM (4 quadrants) ----------
$Seen = New-Object 'System.Collections.Generic.HashSet[long]'
$SbPkO = New-Object System.Text.StringBuilder
$NbPkO = 0; $NbOuverts = 0; $NbName = 0; $NbAccess = 0
$SampleAccent = $null
for ($Ty = 0; $Ty -lt 2; $Ty++) {
	for ($Tx = 0; $Tx -lt 2; $Tx++) {
		$TS = $B.S + ($B.N - $B.S) * $Ty / 2; $TN = $B.S + ($B.N - $B.S) * ($Ty + 1) / 2
		$TW = $B.W + ($B.E - $B.W) * $Tx / 2; $TE = $B.W + ($B.E - $B.W) * ($Tx + 1) / 2
		$Q = @"
[out:json][timeout:180];
way["amenity"="parking"]($TS,$TW,$TN,$TE);
out geom;
"@
		$O = GFOverpass $Q $Theme "parkings quadrant ($Tx,$Ty)"
		foreach ($El in $O.elements) {
			if ($El.type -ne 'way' -or -not $El.geometry) { continue }
			if (-not $Seen.Add([long]$El.id)) { continue }
			$First = $El.geometry[0]; $Last = $El.geometry[$El.geometry.Count - 1]
			if ([Math]::Abs($First.lat - $Last.lat) -gt 1e-7 -or [Math]::Abs($First.lon - $Last.lon) -gt 1e-7) { $NbOuverts++; continue }
			$Ring = @(); foreach ($G in $El.geometry) { $Ring += , @($G.lon, $G.lat) }
			$Pts = GFThinRing $Ring 2.0 400
			if ($Pts.Count -lt 3) { continue }
			if ($NbPkO -gt 0) { [void]$SbPkO.Append(',') }
			[void]$SbPkO.Append('{"pts":')
			GFAppendPts $SbPkO $Pts
			if ($El.tags.parking) { [void]$SbPkO.Append(',"parking":"').Append((GFEsc "$($El.tags.parking)")).Append('"') }
			if ($El.tags.name) {
				$Name = "$($El.tags.name)"
				[void]$SbPkO.Append(',"name":"').Append((GFEsc $Name)).Append('"')
				$NbName++
				if (-not $SampleAccent -and $Name -match '[^\x00-\x7f]') { $SampleAccent = "parking OSM name=$Name X=$(F $Pts[0][0]) Y=$(F $Pts[0][1])" }
			}
			if ($El.tags.access) { [void]$SbPkO.Append(',"access":"').Append((GFEsc "$($El.tags.access)")).Append('"'); $NbAccess++ }
			[void]$SbPkO.Append('}')
			GFBoundsPts $Bd $Pts
			$NbPkO++
		}
		GFLog $Theme "parkings quadrant ($Tx,$Ty) : cumul $NbPkO"
		Start-Sleep 2
	}
}

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"terrains_sport":[')
[void]$Out.Append($SbSport.ToString()).Append('],"cimetieres":[').Append($SbCim.ToString())
[void]$Out.Append('],"parkings_bdtopo":[').Append($SbPkB.ToString())
[void]$Out.Append('],"parkings_osm":[').Append($SbPkO.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$PctName = 0.0; if ($NbPkO -gt 0) { $PctName = 100.0 * $NbName / $NbPkO }
$PctAccess = 0.0; if ($NbPkO -gt 0) { $PctAccess = 100.0 * $NbAccess / $NbPkO }
$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## equipements.json -- terrains de sport, cimetieres, parkings')
$L.Add("- Sources : WFS Geoplateforme BDTOPO_V3:terrain_de_sport, BDTOPO_V3:cimetiere, BDTOPO_V3:equipement_de_transport filtre nature~parking (Licence Ouverte 2.0 IGN) ; OSM way[amenity=parking] (ODbL 1.0). Fetch du $(Get-Date -Format 'yyyy-MM-dd').")
$L.Add("- Objets : $NbSport terrains de sport, $NbCim cimetieres, $NbPkB parkings BD TOPO, $NbPkO parkings OSM ($NbOuverts ways ouverts ecartes).")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage terrains de sport :')
if ($KeysSport) { $L.Add((GFFillText $KeysSport $FillSport $NbSport)) }
$L.Add('- Taux de remplissage cimetieres :')
if ($KeysCim) { $L.Add((GFFillText $KeysCim $FillCim $NbCim)) }
$L.Add('- Taux de remplissage parkings BD TOPO :')
if ($KeysPkB) { $L.Add((GFFillText $KeysPkB $FillPkB $NbPkB)) } else { $L.Add('  - (aucun parking BD TOPO trouve via equipement_de_transport)') }
$L.Add('- Parkings OSM : name ' + $NbName + ' / ' + $NbPkO + ' (' + $PctName.ToString('0.0', $Inv) + ' %), access ' + $NbAccess + ' / ' + $NbPkO + ' (' + $PctAccess.ToString('0.0', $Inv) + ' %).')
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
if ($SampleAccent) { $L.Add("- Temoin (accent) : $SampleAccent") }
$L.Add('- RAPPEL : pas de couche parking dediee dans BD TOPO -- les parkings IGN viennent d equipement_de_transport (couverture partielle, surtout grands parkings).')
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : sport=$NbSport cimetieres=$NbCim parkingsBD=$NbPkB parkingsOSM=$NbPkO, $SizeMo Mo"
