# Fetch-GF-Cours.ps1 -- GRAND FETCH : les COURS interieures des batiments (BD TOPO).
# Meme couche et meme bbox que Fetch-GF-BatiEnrichi.ps1 (BDTOPO_V3:batiment, +-5000 m),
# mais on garde les ANNEAUX INTERIEURS (trous = cours) que GFExteriorRings jette.
# Produit cours.json : la liste des SEULS batiments qui ont au moins une cour, chacun
# avec son emprise exterieure (pour le matching en aval) et la geometrie de ses cours,
# le tout en coordonnees LOCALES (ToLocalX/ToLocalY, NORD = -Y).
#
# Convention emprise = polygone [0] de la geometrie, IDENTIQUE a Fetch-GF-BatiEnrichi.ps1
# (qui prend $Rings[0]) : le matching 1:1 batiment<->cours en depend.
#
# Filtres :
#   - "En service" seulement (meme filtre que BatiEnrichi) ;
#   - trous d'aire >= 15 m2 (en dessous = bruit de numerisation, pas une vraie cour).
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'cours'
$Layer = 'BDTOPO_V3:batiment'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'cours.json'

$SeuilExt = 0.8   # amincissement emprise exterieure (comme BatiEnrichi)
$SeuilTrou = 0.5  # amincissement des cours : plus fin (elles sont petites)
$AireMin = 15.0   # m2 : seuil "vraie cour"

$Sb = New-Object System.Text.StringBuilder
$Nb = 0            # batiments a cour retenus
$NbTrous = 0       # cours retenues
$NbTrousRejet = 0  # trous ecartes (aire < seuil ou < 3 pts)
$AireTot = 0.0     # m2 de cours cumulee
$NbLus = 0; $NbHorsService = 0

GFLog $Theme "DEBUT ($Layer, bbox +-5000 m, trous >= $AireMin m2)"
$Start = 0
do {
	$R = GFWfsPage $Layer $B $Start $Theme
	$Feats = @($R.features)
	foreach ($Feat in $Feats) {
		$NbLus++
		$Props = $Feat.properties
		if ($Props.etat_de_l_objet -and $Props.etat_de_l_objet -ne 'En service') { $NbHorsService++; continue }
		$Polys = GFRingsWithHoles $Feat.geometry
		if ($Polys.Count -lt 1) { continue }
		# Convention BatiEnrichi : le batiment = le PREMIER polygone.
		$Poly = $Polys[0]
		if (@($Poly).Count -lt 2) { continue }   # pas de trou dans le polygone principal
		# Emprise exterieure locale (pour le matching centroide/recouvrement en aval).
		$Ext = GFThinRing $Poly[0] $SeuilExt 120
		if ($Ext.Count -lt 3) { continue }
		# Cours : chaque anneau interieur, aminci, garde si aire >= seuil.
		$Trous = New-Object System.Collections.Generic.List[object]
		for ($k = 1; $k -lt @($Poly).Count; $k++) {
			$H = GFThinRing $Poly[$k] $SeuilTrou 120
			if ($H.Count -lt 3) { $NbTrousRejet++; continue }
			$Aire = GFRingAreaM2 $H
			if ($Aire -lt $AireMin) { $NbTrousRejet++; continue }
			$Trous.Add(@{ Pts = $H; Aire = $Aire })
		}
		if ($Trous.Count -lt 1) { continue }
		if ($Nb -gt 0) { [void]$Sb.Append(',') }
		[void]$Sb.Append('{"pts":')
		GFAppendPts $Sb $Ext
		[void]$Sb.Append(',"holes":[')
		for ($t = 0; $t -lt $Trous.Count; $t++) {
			if ($t -gt 0) { [void]$Sb.Append(',') }
			GFAppendPts $Sb $Trous[$t].Pts
			$NbTrous++
			$AireTot += $Trous[$t].Aire
		}
		[void]$Sb.Append(']}')
		$Nb++
	}
	$Got = $Feats.Count
	GFLog $Theme "page STARTINDEX=$Start : $Got lus, cumul $Nb bat a cour ($NbTrous cours)"
	$Start += 1000
	Start-Sleep -Milliseconds 1200
} while ($Got -eq 1000)

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"source":"Fetch-GF-Cours.ps1 (BDTOPO_V3:batiment, anneaux interieurs)",')
[void]$Out.Append('"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},')
[void]$Out.Append('"aireMinM2":').Append((F $AireMin)).Append(',"buildings":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeKo = [Math]::Round((Get-Item $OutPath).Length / 1KB, 1)

GFLog $Theme "FIN : $Nb batiments a cour, $NbTrous cours (>= $AireMin m2), aire totale $([Math]::Round($AireTot)) m2, $NbTrousRejet trous ecartes, $NbLus lus, $NbHorsService hors service, $SizeKo Ko -> $OutPath"

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## cours.json -- cours interieures des batiments')
$L.Add("- Source : WFS Geoplateforme $Layer (BD TOPO v3), fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte 2.0 (IGN).")
$L.Add("- Batiments a cour : $Nb (sur $NbLus lus), $NbTrous cours retenues (>= $AireMin m2), $NbTrousRejet ecartees.")
$L.Add("- Aire de cours cumulee : $([Math]::Round($AireTot)) m2. Taille : $SizeKo Ko.")
GFRapportAdd (($L -join "`r`n"))
