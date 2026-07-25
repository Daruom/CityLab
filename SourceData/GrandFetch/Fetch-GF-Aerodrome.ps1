# Fetch-GF-Aerodrome.ps1 -- GRAND FETCH theme 4 : aerodromes (BD TOPO).
# BBOX ETENDUE +-9000 m : l'aeroport de Blagnac (~43.63, 1.36) est HORS de la map
# 10 km mais on veut ses donnees. Des coordonnees sortent donc de +-5000 (assume).
# Couches : BDTOPO_V3:aerodrome (emprises) + BDTOPO_V3:piste_d_aerodrome (pistes).
# L'inventaire GetCapabilities ne montre AUCUNE couche de zone associee.
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'aerodrome'
$B9 = GFBbox 9000.0
$OutPath = Join-Path $GFDir 'aerodrome.json'
$Bd = GFNewBounds
GFLog $Theme 'DEBUT (BDTOPO_V3:aerodrome + piste_d_aerodrome, bbox ETENDUE +-9000 m)'

function EmitSurfaces([string]$Layer, [double]$Seuil, [System.Text.StringBuilder]$Sb, $Fill, [ref]$KeysRef, [ref]$NbRef2, [string]$Theme2) {
	$Nb = 0
	foreach ($Feat in (GFWfsAll $Layer (GFBbox 9000.0) $Theme2)) {
		if ($null -eq $KeysRef.Value) { $KeysRef.Value = GFScalarKeys $Feat.properties }
		$Rings = GFExteriorRings $Feat.geometry
		$SbR = New-Object System.Text.StringBuilder
		$NbR = 0
		foreach ($Ring in $Rings) {
			$Pts = GFThinRing $Ring $Seuil 400
			if ($Pts.Count -lt 3) { continue }
			if ($NbR -gt 0) { [void]$SbR.Append(',') }
			GFAppendPts $SbR $Pts
			GFBoundsPts $script:Bd $Pts
			$NbR++
		}
		if ($NbR -lt 1) { continue }
		if ($Nb -gt 0) { [void]$Sb.Append(',') }
		[void]$Sb.Append('{"rings":[').Append($SbR.ToString()).Append(']')
		GFAppendPropsCount $Sb $Feat.properties $KeysRef.Value $Fill
		[void]$Sb.Append('}')
		$Nb++
	}
	$NbRef2.Value = $Nb
}

$SbA = New-Object System.Text.StringBuilder
$KeysA = $null; $FillA = @{}; $NbA = 0
EmitSurfaces 'BDTOPO_V3:aerodrome' 8.0 $SbA $FillA ([ref]$KeysA) ([ref]$NbA) $Theme
GFLog $Theme "aerodromes : $NbA emprises"

$SbP = New-Object System.Text.StringBuilder
$KeysP = $null; $FillP = @{}; $NbP = 0
EmitSurfaces 'BDTOPO_V3:piste_d_aerodrome' 2.0 $SbP $FillP ([ref]$KeysP) ([ref]$NbP) $Theme
GFLog $Theme "pistes : $NbP surfaces"

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"halfM":9000,"aerodromes":[')
[void]$Out.Append($SbA.ToString()).Append('],"pistes":[').Append($SbP.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## aerodrome.json -- aerodromes (bbox ETENDUE +-9000 m)')
$L.Add("- Sources : WFS Geoplateforme BDTOPO_V3:aerodrome et BDTOPO_V3:piste_d_aerodrome (BD TOPO v3), fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte 2.0 (IGN).")
$L.Add("- Objets : $NbA emprises d'aerodrome, $NbP surfaces de piste.")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage aerodromes :')
if ($KeysA) { $L.Add((GFFillText $KeysA $FillA $NbA)) } else { $L.Add('  - (aucune emprise trouvee)') }
$L.Add('- Taux de remplissage pistes :')
if ($KeysP) { $L.Add((GFFillText $KeysP $FillP $NbP)) } else { $L.Add('  - (aucune piste trouvee)') }
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- ASSUME : bbox +-9000 m, des coordonnees SORTENT de +-5000 m (Blagnac est hors map 10 km, donnees voulues quand meme).')
$L.Add('- Aucune couche de zone associee aux aerodromes dans les capabilities (verifie a l etape 0).')
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $NbA aerodromes + $NbP pistes, $SizeMo Mo"
