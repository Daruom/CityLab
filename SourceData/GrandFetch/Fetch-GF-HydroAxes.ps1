# Fetch-GF-HydroAxes.ps1 -- GRAND FETCH theme 10 : axes des cours d'eau (BD TOPO).
# BDTOPO_V3:troncon_hydrographique : polylignes (amincies 2 m) + attributs BRUTS
# (nature, classe, position par rapport au sol, toponyme du cours d'eau...).
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'hydro_axes'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'hydro_axes.json'
$Bd = GFNewBounds
GFLog $Theme 'DEBUT (BDTOPO_V3:troncon_hydrographique)'

$Sb = New-Object System.Text.StringBuilder
$Keys = $null; $Fill = @{}; $Nb = 0; $NbSkip = 0; $NbNommes = 0
$Samples = New-Object System.Collections.Generic.List[string]
$SampleAccent = $null

foreach ($Feat in (GFWfsAll 'BDTOPO_V3:troncon_hydrographique' $B $Theme)) {
	if ($null -eq $Keys) {
		$Keys = GFScalarKeys $Feat.properties
		GFLog $Theme ('attributs retenus : ' + ($Keys -join ', '))
	}
	$Props = $Feat.properties
	$Nom = "$($Props.cpx_toponyme_de_cours_d_eau)"
	if ($Nom -eq '') { $Nom = "$($Props.toponyme)" }
	$NbEmis = 0
	foreach ($Line in (GFLines $Feat.geometry)) {
		$Pts = GFThinLine $Line 2.0
		if ($Pts.Count -lt 2) { continue }
		if ($Nb -gt 0) { [void]$Sb.Append(',') }
		[void]$Sb.Append('{"pts":')
		GFAppendPts $Sb $Pts
		GFAppendPropsCount $Sb $Props $Keys $Fill
		[void]$Sb.Append('}')
		GFBoundsPts $Bd $Pts
		$Nb++; $NbEmis++
		if ($Nom -ne '') {
			if ($Samples.Count -lt 2) { $Samples.Add("nom=$Nom nature=$($Props.nature) pts=$($Pts.Count) X=$(F $Pts[0][0]) Y=$(F $Pts[0][1])") }
			if (-not $SampleAccent -and $Nom -match '[^\x00-\x7f]') { $SampleAccent = "nom=$Nom nature=$($Props.nature) X=$(F $Pts[0][0]) Y=$(F $Pts[0][1])" }
		}
	}
	if ($NbEmis -eq 0) { $NbSkip++ } elseif ($Nom -ne '') { $NbNommes++ }
}

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"troncons":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## hydro_axes.json -- axes des cours d eau')
$L.Add("- Source : WFS Geoplateforme BDTOPO_V3:troncon_hydrographique (BD TOPO v3), fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte 2.0 (IGN).")
$L.Add("- Objets : $Nb polylignes ($NbSkip features sans polyligne exploitable) ; $NbNommes features avec toponyme de cours d eau.")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage :')
if ($Keys) { $L.Add((GFFillText $Keys $Fill $Nb)) }
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- Temoins :')
foreach ($S in $Samples) { $L.Add("  - $S") }
if ($SampleAccent) { $L.Add("  - (accent) $SampleAccent") }
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $Nb troncons, $SizeMo Mo"
