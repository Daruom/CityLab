# Fetch-GF-Ferre.ps1 -- GRAND FETCH theme 6 : reseau ferre.
# 1) BDTOPO_V3:troncon_de_voie_ferree : polylignes + attributs BRUTS (nature,
#    electrifie, nombre_de_voies, position_par_rapport_au_sol...).
# 2) BDTOPO_V3:equipement_de_transport filtre nature gare/station/halte (pas de
#    couche gare dediee dans les capabilities).
# 3) OSM Overpass : node/way railway station|halt|tram_stop|subway_entrance avec name.
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'ferre'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'ferre.json'
$Bd = GFNewBounds
GFLog $Theme 'DEBUT (troncon_de_voie_ferree + equipement_de_transport + stations OSM)'

# ---------- Troncons BD TOPO ----------
$SbT = New-Object System.Text.StringBuilder
$KeysT = $null; $FillT = @{}; $NbT = 0
foreach ($Feat in (GFWfsAll 'BDTOPO_V3:troncon_de_voie_ferree' $B $Theme)) {
	if ($null -eq $KeysT) {
		$KeysT = GFScalarKeys $Feat.properties
		GFLog $Theme ('attributs troncons : ' + ($KeysT -join ', '))
	}
	foreach ($Line in (GFLines $Feat.geometry)) {
		$Pts = GFThinLine $Line 2.0
		if ($Pts.Count -lt 2) { continue }
		if ($NbT -gt 0) { [void]$SbT.Append(',') }
		[void]$SbT.Append('{"pts":')
		GFAppendPts $SbT $Pts
		GFAppendPropsCount $SbT $Feat.properties $KeysT $FillT
		[void]$SbT.Append('}')
		GFBoundsPts $Bd $Pts
		$NbT++
	}
}
GFLog $Theme "troncons ferres : $NbT"

# ---------- Gares BD TOPO (equipement_de_transport filtre) ----------
$SbG = New-Object System.Text.StringBuilder
$KeysG = $null; $FillG = @{}; $NbG = 0; $NbEquTotal = 0
foreach ($Feat in (GFWfsAll 'BDTOPO_V3:equipement_de_transport' $B $Theme)) {
	$NbEquTotal++
	$Props = $Feat.properties
	if ("$($Props.nature)" -notmatch 'gare|station|halte') { continue }
	if ($null -eq $KeysG) { $KeysG = GFScalarKeys $Props }
	$X = $null; $Y = $null
	$G = $Feat.geometry
	if ($G -and $G.type -eq 'Point') {
		$X = ToLocalX $G.coordinates[0]; $Y = ToLocalY $G.coordinates[1]
	} else {
		$Rings = GFExteriorRings $G
		if ($Rings.Count -ge 1) {
			$SumX = 0.0; $SumY = 0.0; $NPt = 0
			foreach ($C in $Rings[0]) { $SumX += (ToLocalX $C[0]); $SumY += (ToLocalY $C[1]); $NPt++ }
			if ($NPt -gt 0) { $X = [Math]::Round($SumX / $NPt, 2); $Y = [Math]::Round($SumY / $NPt, 2) }
		}
	}
	if ($null -eq $X) { continue }
	if ($NbG -gt 0) { [void]$SbG.Append(',') }
	[void]$SbG.Append('{"x":').Append((F $X)).Append(',"y":').Append((F $Y))
	GFAppendPropsCount $SbG $Props $KeysG $FillG
	[void]$SbG.Append('}')
	GFBoundsPts $Bd @(, @($X, $Y))
	$NbG++
}
GFLog $Theme "equipements gare/station/halte BD TOPO : $NbG (sur $NbEquTotal equipements de transport)"

# ---------- Stations OSM ----------
$Seen = New-Object 'System.Collections.Generic.HashSet[string]'
$SbS = New-Object System.Text.StringBuilder
$NbS = 0
$CountByType = @{}
$Samples = New-Object System.Collections.Generic.List[string]
$SampleAccent = $null
$Q = @"
[out:json][timeout:180];
(
  node["railway"~"^(station|halt|tram_stop|subway_entrance)$"]["name"]($($B.S),$($B.W),$($B.N),$($B.E));
  way["railway"~"^(station|halt|tram_stop|subway_entrance)$"]["name"]($($B.S),$($B.W),$($B.N),$($B.E));
);
out center;
"@
$O = GFOverpass $Q $Theme 'stations'
foreach ($El in $O.elements) {
	if (-not $Seen.Add("$($El.type)/$($El.id)")) { continue }
	$Lat = $null; $Lon = $null
	if ($El.type -eq 'node') { $Lat = $El.lat; $Lon = $El.lon }
	elseif ($El.center) { $Lat = $El.center.lat; $Lon = $El.center.lon }
	if ($null -eq $Lat) { continue }
	$Name = "$($El.tags.name)"
	$RType = "$($El.tags.railway)"
	$X = ToLocalX $Lon; $Y = ToLocalY $Lat
	if ($NbS -gt 0) { [void]$SbS.Append(',') }
	[void]$SbS.Append('{"x":').Append((F $X)).Append(',"y":').Append((F $Y))
	[void]$SbS.Append(',"name":"').Append((GFEsc $Name)).Append('","railway":"').Append((GFEsc $RType)).Append('"}')
	GFBoundsPts $Bd @(, @($X, $Y))
	if ($CountByType.ContainsKey($RType)) { $CountByType[$RType] = $CountByType[$RType] + 1 } else { $CountByType[$RType] = 1 }
	if ($Samples.Count -lt 2 -and $RType -eq 'station') { $Samples.Add("$RType $Name X=$(F $X) Y=$(F $Y)") }
	if (-not $SampleAccent -and $Name -match '[^\x00-\x7f]') { $SampleAccent = "$RType $Name X=$(F $X) Y=$(F $Y)" }
	$NbS++
}
GFLog $Theme "stations OSM : $NbS"

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"troncons":[')
[void]$Out.Append($SbT.ToString()).Append('],"equipements_bdtopo":[').Append($SbG.ToString())
[void]$Out.Append('],"stations_osm":[').Append($SbS.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$Types = New-Object System.Collections.Generic.List[string]
foreach ($K in ($CountByType.Keys | Sort-Object)) { $Types.Add("$K=$($CountByType[$K])") }
$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## ferre.json -- reseau ferre')
$L.Add("- Sources : WFS Geoplateforme BDTOPO_V3:troncon_de_voie_ferree + BDTOPO_V3:equipement_de_transport (Licence Ouverte 2.0 IGN) ; OSM Overpass stations (ODbL 1.0). Fetch du $(Get-Date -Format 'yyyy-MM-dd').")
$L.Add("- Objets : $NbT polylignes ferrees ; $NbG equipements BD TOPO nature gare/station/halte (sur $NbEquTotal equipements de transport de la bbox) ; $NbS stations OSM nommees (" + ($Types -join ', ') + ').')
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage troncons :')
if ($KeysT) { $L.Add((GFFillText $KeysT $FillT $NbT)) }
$L.Add('- Taux de remplissage equipements BD TOPO :')
if ($KeysG) { $L.Add((GFFillText $KeysG $FillG $NbG)) } else { $L.Add('  - (aucun equipement gare/station/halte)') }
$L.Add('- Stations OSM : name 100 % (filtre requete). ATTENTION : les subway_entrance sans tag name (frequent) sont ABSENTES par design.')
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- Temoins :')
foreach ($S in $Samples) { $L.Add("  - $S") }
if ($SampleAccent) { $L.Add("  - (accent) $SampleAccent") }
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $NbT troncons + $NbG equipements + $NbS stations, $SizeMo Mo"
