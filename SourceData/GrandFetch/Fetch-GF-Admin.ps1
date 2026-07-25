# Fetch-GF-Admin.ps1 -- GRAND FETCH theme 11 : limites administratives.
# ADMINEXPRESS-COG.LATEST:commune intersectant la bbox standard : nom, code INSEE,
# population (si dispo), polygones amincis 8 m (seuil des surfaces historiques).
# Les polygones des communes DEBORDENT naturellement de +-5000 m (communes entieres).
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'admin'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'admin.json'
$Bd = GFNewBounds
GFLog $Theme 'DEBUT (ADMINEXPRESS-COG.LATEST:commune)'

$Sb = New-Object System.Text.StringBuilder
$Keys = $null; $Fill = @{}; $Nb = 0; $NbSkip = 0
$Noms = New-Object System.Collections.Generic.List[string]
$SampleAccent = $null

foreach ($Feat in (GFWfsAll 'ADMINEXPRESS-COG.LATEST:commune' $B $Theme)) {
	if ($null -eq $Keys) {
		$Keys = GFScalarKeys $Feat.properties
		GFLog $Theme ('attributs retenus : ' + ($Keys -join ', '))
	}
	$Props = $Feat.properties
	$Rings = GFExteriorRings $Feat.geometry
	$SbR = New-Object System.Text.StringBuilder
	$NbR = 0
	foreach ($Ring in $Rings) {
		$Pts = GFThinRing $Ring 8.0 400
		if ($Pts.Count -lt 3) { continue }
		if ($NbR -gt 0) { [void]$SbR.Append(',') }
		GFAppendPts $SbR $Pts
		GFBoundsPts $Bd $Pts
		$NbR++
	}
	if ($NbR -lt 1) { $NbSkip++; continue }
	if ($Nb -gt 0) { [void]$Sb.Append(',') }
	[void]$Sb.Append('{"rings":[').Append($SbR.ToString()).Append(']')
	GFAppendPropsCount $Sb $Props $Keys $Fill
	[void]$Sb.Append('}')
	$Nom = "$($Props.nom)"
	if ($Nom -eq '') { $Nom = "$($Props.nom_officiel)" }
	$Pop = "$($Props.population)"
	$Noms.Add("$Nom (INSEE $($Props.insee_com), pop $Pop)")
	if (-not $SampleAccent -and $Nom -match '[^\x00-\x7f]') { $SampleAccent = "$Nom (INSEE $($Props.insee_com), pop $Pop)" }
	$Nb++
}

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"communes":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## admin.json -- communes')
$L.Add("- Source : WFS Geoplateforme ADMINEXPRESS-COG.LATEST:commune (ADMIN EXPRESS COG), fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte 2.0 (IGN).")
$L.Add("- Objets : $Nb communes intersectant la bbox ($NbSkip sans anneau exploitable). Anneaux amincis 8 m / plafond 400 pts.")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage :')
if ($Keys) { $L.Add((GFFillText $Keys $Fill $Nb)) }
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- ASSUME : les bornes DEBORDENT de +-5000 m (les communes intersectantes sont gardees entieres).')
$L.Add('- Communes retenues :')
foreach ($S in $Noms) { $L.Add("  - $S") }
if ($SampleAccent) { $L.Add("  - (accent) $SampleAccent") }
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $Nb communes, $SizeMo Mo"
