# Fetch-GF-BatiEnrichi.ps1 -- GRAND FETCH theme 1 : batiments enrichis (BD TOPO).
# Tous les batiments "En service" de la bbox standard +-5000 m. Emprise = anneau
# exterieur aminci (seuil 0.8 m comme Fetch-Toulouse10.ps1, plafond 120 pts par
# DECIMATION : aucun batiment ecarte pour complexite). Attributs conserves BRUTS
# (hauteur, altitudes sol/toit -- l'entree du chantier toits J3b --, materiaux,
# usages, etages). AUCUN filtrage gameplay : les builders decideront.
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'bati_enrichi'
$Layer = 'BDTOPO_V3:batiment'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'bati_enrichi.json'

$KeyWanted = '^(nature|usage_1|usage_2|hauteur|nombre_d_etages|nombre_de_logements|altitude_minimale_sol|altitude_maximale_sol|altitude_minimale_toit|altitude_maximale_toit|materiaux_des_murs|materiaux_de_la_toiture|construction_legere|legerete|etat_de_l_objet|origine_du_batiment)$'
$Keys = $null
$Fill = @{}
$Sb = New-Object System.Text.StringBuilder
$Bd = GFNewBounds
$Nb = 0; $NbLus = 0; $NbHorsService = 0; $NbSansGeom = 0; $NbDecimes = 0
$Samples = New-Object System.Collections.Generic.List[string]
$SampleAccent = $null

GFLog $Theme "DEBUT ($Layer, bbox +-5000 m)"
$Start = 0
do {
	$R = GFWfsPage $Layer $B $Start $Theme
	$Feats = @($R.features)
	if ($null -eq $Keys -and $Feats.Count -gt 0) {
		$Keys = @()
		foreach ($P in $Feats[0].properties.PSObject.Properties) {
			if ($P.Name -match $KeyWanted) { $Keys += $P.Name }
		}
		GFLog $Theme ('attributs retenus : ' + ($Keys -join ', '))
	}
	foreach ($Feat in $Feats) {
		$NbLus++
		$Props = $Feat.properties
		if ($Props.etat_de_l_objet -and $Props.etat_de_l_objet -ne 'En service') { $NbHorsService++; continue }
		$Rings = GFExteriorRings $Feat.geometry
		if ($Rings.Count -lt 1) { $NbSansGeom++; continue }
		$Ring = $Rings[0]
		if (-not $Ring -or @($Ring).Count -lt 4) { $NbSansGeom++; continue }
		# Amincissement 0.8 m, pattern identique au script canonique.
		$Pts = New-Object System.Collections.Generic.List[object]
		$Prev = $null
		foreach ($C in $Ring) {
			$P = @((ToLocalX $C[0]), (ToLocalY $C[1]))
			if ($Prev -and ([Math]::Abs($P[0]-$Prev[0]) + [Math]::Abs($P[1]-$Prev[1])) -lt 0.8) { continue }
			$Pts.Add($P); $Prev = $P
		}
		if ($Pts.Count -gt 1) {
			$A = $Pts[0]; $Z = $Pts[$Pts.Count-1]
			if (([Math]::Abs($A[0]-$Z[0]) + [Math]::Abs($A[1]-$Z[1])) -lt 0.8) { $Pts.RemoveAt($Pts.Count-1) }
		}
		if ($Pts.Count -lt 3) { $NbSansGeom++; continue }
		if ($Pts.Count -gt 120) {
			$Step = [Math]::Ceiling($Pts.Count / 120.0)
			$Thin = New-Object System.Collections.Generic.List[object]
			for ($i = 0; $i -lt $Pts.Count; $i += $Step) { $Thin.Add($Pts[$i]) }
			$Pts = $Thin
			$NbDecimes++
		}
		if ($Nb -gt 0) { [void]$Sb.Append(',') }
		[void]$Sb.Append('{"pts":')
		GFAppendPts $Sb $Pts
		GFAppendPropsCount $Sb $Props $Keys $Fill
		[void]$Sb.Append('}')
		GFBoundsPts $Bd $Pts
		if ($Samples.Count -lt 2 -and $Props.altitude_maximale_toit -and $Props.materiaux_de_la_toiture) {
			$Samples.Add("nature=$($Props.nature) usage_1=$($Props.usage_1) hauteur=$($Props.hauteur) alt_toit_min=$($Props.altitude_minimale_toit) alt_toit_max=$($Props.altitude_maximale_toit) mat_toit=$($Props.materiaux_de_la_toiture) mat_murs=$($Props.materiaux_des_murs) pts=$($Pts.Count) X=$(F $Pts[0][0]) Y=$(F $Pts[0][1])")
		}
		if (-not $SampleAccent -and ("$($Props.usage_1)" -match '[^\x00-\x7f]' -or "$($Props.nature)" -match '[^\x00-\x7f]')) {
			$SampleAccent = "nature=$($Props.nature) usage_1=$($Props.usage_1) usage_2=$($Props.usage_2) hauteur=$($Props.hauteur) etages=$($Props.nombre_d_etages) X=$(F $Pts[0][0]) Y=$(F $Pts[0][1])"
		}
		$Nb++
	}
	$Got = $Feats.Count
	GFLog $Theme "page STARTINDEX=$Start : $Got lus, cumul $Nb retenus"
	$Start += 1000
	Start-Sleep -Milliseconds 1200
} while ($Got -eq 1000)

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"buildings":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 1)

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## bati_enrichi.json -- batiments enrichis')
$L.Add("- Source : WFS Geoplateforme $Layer (BD TOPO v3), fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte 2.0 (IGN).")
$L.Add("- Objets : $Nb batiments En service retenus sur $NbLus lus ($NbHorsService hors service ecartes, $NbSansGeom sans geometrie exploitable, $NbDecimes anneaux decimes au plafond de 120 pts).")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage des attributs (batiments retenus) :')
$L.Add((GFFillText $Keys $Fill $Nb))
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- Temoins :')
foreach ($S in $Samples) { $L.Add("  - $S") }
if ($SampleAccent) { $L.Add("  - (accent) $SampleAccent") }
$L.Add("- Note : plafond 120 pts par decimation (le script historique ECARTAIT les anneaux > 80 pts ; ici on ne perd aucun objet, campagne d'acquisition oblige).")
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $Nb batiments, $SizeMo Mo"
