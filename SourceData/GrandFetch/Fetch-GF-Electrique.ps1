# Fetch-GF-Electrique.ps1 -- GRAND FETCH theme 3 : reseau electrique (BD TOPO).
# BDTOPO_V3:pylone (points + attributs bruts dont hauteur) et
# BDTOPO_V3:ligne_electrique (polylignes + attributs bruts dont voltage).
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'electrique'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'electrique.json'
$Bd = GFNewBounds
GFLog $Theme 'DEBUT (BDTOPO_V3:pylone + ligne_electrique)'

# ---------- Pylones (points) ----------
$SbP = New-Object System.Text.StringBuilder
$KeysP = $null; $FillP = @{}; $NbP = 0; $NbPSkip = 0
$TemoinP = $null
foreach ($Feat in (GFWfsAll 'BDTOPO_V3:pylone' $B $Theme)) {
	if ($null -eq $KeysP) { $KeysP = GFScalarKeys $Feat.properties }
	$G = $Feat.geometry
	if ($null -eq $G -or $G.type -ne 'Point') { $NbPSkip++; continue }
	$P = @((ToLocalX $G.coordinates[0]), (ToLocalY $G.coordinates[1]))
	if ($NbP -gt 0) { [void]$SbP.Append(',') }
	[void]$SbP.Append('{"x":').Append((F $P[0])).Append(',"y":').Append((F $P[1]))
	GFAppendPropsCount $SbP $Feat.properties $KeysP $FillP
	[void]$SbP.Append('}')
	GFBoundsPts $Bd @(, $P)
	if (-not $TemoinP -and $Feat.properties.hauteur) { $TemoinP = "pylone hauteur=$($Feat.properties.hauteur) nature=$($Feat.properties.nature) X=$(F $P[0]) Y=$(F $P[1])" }
	$NbP++
}
GFLog $Theme "pylones : $NbP retenus ($NbPSkip geometrie non ponctuelle)"

# ---------- Lignes electriques (polylignes) ----------
$SbL = New-Object System.Text.StringBuilder
$KeysL = $null; $FillL = @{}; $NbL = 0; $NbLSkip = 0
$TemoinL = $null; $TemoinAccent = $null
foreach ($Feat in (GFWfsAll 'BDTOPO_V3:ligne_electrique' $B $Theme)) {
	if ($null -eq $KeysL) { $KeysL = GFScalarKeys $Feat.properties }
	$NbEmis = 0
	foreach ($Line in (GFLines $Feat.geometry)) {
		$Pts = GFThinLine $Line 2.0
		if ($Pts.Count -lt 2) { continue }
		if ($NbL -gt 0) { [void]$SbL.Append(',') }
		[void]$SbL.Append('{"pts":')
		GFAppendPts $SbL $Pts
		GFAppendPropsCount $SbL $Feat.properties $KeysL $FillL
		[void]$SbL.Append('}')
		GFBoundsPts $Bd $Pts
		if (-not $TemoinL -and $Feat.properties.voltage) { $TemoinL = "ligne voltage=$($Feat.properties.voltage) nature=$($Feat.properties.nature) pts=$($Pts.Count) X=$(F $Pts[0][0]) Y=$(F $Pts[0][1])" }
		if (-not $TemoinAccent -and ("$($Feat.properties.nature)" -match '[^\x00-\x7f]' -or "$($Feat.properties.etat_de_l_objet)" -match '[^\x00-\x7f]')) {
			$TemoinAccent = "ligne nature=$($Feat.properties.nature) etat=$($Feat.properties.etat_de_l_objet) voltage=$($Feat.properties.voltage)"
		}
		$NbL++; $NbEmis++
	}
	if ($NbEmis -eq 0) { $NbLSkip++ }
}
GFLog $Theme "lignes : $NbL polylignes retenues ($NbLSkip features sans polyligne)"

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"pylones":[')
[void]$Out.Append($SbP.ToString()).Append('],"lignes":[').Append($SbL.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## electrique.json -- reseau electrique')
$L.Add("- Sources : WFS Geoplateforme BDTOPO_V3:pylone et BDTOPO_V3:ligne_electrique (BD TOPO v3), fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte 2.0 (IGN).")
$L.Add("- Objets : $NbP pylones, $NbL polylignes de ligne electrique.")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage pylones :')
if ($KeysP) { $L.Add((GFFillText $KeysP $FillP $NbP)) } else { $L.Add('  - (aucun pylone dans la bbox)') }
$L.Add('- Taux de remplissage lignes :')
if ($KeysL) { $L.Add((GFFillText $KeysL $FillL $NbL)) } else { $L.Add('  - (aucune ligne dans la bbox)') }
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- Temoins :')
if ($TemoinP) { $L.Add("  - $TemoinP") }
if ($TemoinL) { $L.Add("  - $TemoinL") }
if ($TemoinAccent) { $L.Add("  - (accent) $TemoinAccent") } else { $L.Add('  - (pas de valeur accentuee rencontree sur ce theme : attributs surtout numeriques/enumeres)') }
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $NbP pylones + $NbL lignes, $SizeMo Mo"
