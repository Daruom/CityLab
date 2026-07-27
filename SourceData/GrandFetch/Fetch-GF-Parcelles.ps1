# Fetch-GF-Parcelles.ps1 -- SOLS PAR LE CADASTRE, verrou 1 : parcelles cadastrales.
# CADASTRALPARCELS.PARCELLAIRE_EXPRESS:parcelle sur la dalle +-5000 m.
# Le corridor public = complement des parcelles -> on garde les TROUS (anneaux
# interieurs) : chaque polygone est emis comme {"rings":[exterieur, trou, ...]}.
# Anneaux amincis a 0,5 m (precision cadastrale, on ne lisse pas les angles droits).
# -HalfM : demi-cote de la dalle (5000 = production ; petite valeur = smoke test).
param([double]$HalfM = 5000.0, [string]$Suffixe = '')
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'parcelles'
$GFLogPath = Join-Path $GFDir "parcelles$Suffixe.progress.log"
$B = GFBbox $HalfM
$OutPath = Join-Path $GFDir "parcelles$Suffixe.json"
if ($Suffixe -ne '') { $GFRapportPath = Join-Path $GFDir "smoke_parcelles$Suffixe.md" }
$Bd = GFNewBounds
$T0 = Get-Date
GFLog $Theme "DEBUT (CADASTRALPARCELS.PARCELLAIRE_EXPRESS:parcelle, bbox +-$($HalfM.ToString('0', $Inv)) m)"

$Sb = New-Object System.Text.StringBuilder
$NbFeat = 0        # features WFS lus
$NbPoly = 0        # polygones emis
$NbTrous = 0       # anneaux interieurs conserves
$NbSkip = 0        # features sans polygone exploitable
$NbMulti = 0       # features multipolygones
$Sections = @{}
$Start = 0
do {
	$R = GFWfsPage 'CADASTRALPARCELS.PARCELLAIRE_EXPRESS:parcelle' $B $Start $Theme
	$Feats = @($R.features)
	$Got = $Feats.Count
	foreach ($Feat in $Feats) {
		$NbFeat++
		$Props = $Feat.properties
		$G = $Feat.geometry
		$Polys = @()
		if ($null -ne $G -and $null -ne $G.coordinates) {
			if ($G.type -eq 'Polygon') { $Polys = @(, $G.coordinates) }
			elseif ($G.type -eq 'MultiPolygon') { $Polys = @($G.coordinates); if ($Polys.Count -gt 1) { $NbMulti++ } }
		}
		$Emis = 0
		foreach ($Poly in $Polys) {
			$Rings = @($Poly)
			if ($Rings.Count -lt 1) { continue }
			$Ext = GFThinRing $Rings[0] 0.5 0
			if ($Ext.Count -lt 3) { continue }
			$Holes = New-Object System.Collections.Generic.List[object]
			for ($ri = 1; $ri -lt $Rings.Count; $ri++) {
				$H = GFThinRing $Rings[$ri] 0.5 0
				if ($H.Count -ge 3) { $Holes.Add($H) }
			}
			if ($NbPoly -gt 0) { [void]$Sb.Append(',') }
			[void]$Sb.Append('{"rings":[')
			GFAppendPts $Sb $Ext
			foreach ($H in $Holes) { [void]$Sb.Append(','); GFAppendPts $Sb $H; $NbTrous++ }
			[void]$Sb.Append(']')
			$Idu = "$($Props.idu)"; $Sec = "$($Props.section)"; $Num = "$($Props.numero)"
			if ($Idu -ne '') { [void]$Sb.Append(',"idu":"').Append((GFEsc $Idu)).Append('"') }
			if ($Sec -ne '') { [void]$Sb.Append(',"section":"').Append((GFEsc $Sec)).Append('"') }
			if ($Num -ne '') { [void]$Sb.Append(',"numero":"').Append((GFEsc $Num)).Append('"') }
			if ($null -ne $Props.contenance) { [void]$Sb.Append(',"contenance":').Append(([double]$Props.contenance).ToString('0', $Inv)) }
			[void]$Sb.Append('}')
			GFBoundsPts $Bd $Ext
			$NbPoly++; $Emis++
			if ($Sec -ne '') { if ($Sections.ContainsKey($Sec)) { $Sections[$Sec] = $Sections[$Sec] + 1 } else { $Sections[$Sec] = 1 } }
		}
		if ($Emis -eq 0) { $NbSkip++ }
	}
	GFLog $Theme "page STARTINDEX=$Start : $Got features (cumul feats $NbFeat, polys $NbPoly, trous $NbTrous, buffer $([Math]::Round($Sb.Length/1MB,1)) Mo)"
	$Start += 1000
	Start-Sleep -Milliseconds 1200
} while ($Got -eq 1000)

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"source":"WFS Geoplateforme CADASTRALPARCELS.PARCELLAIRE_EXPRESS:parcelle","origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"parcelles":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)
$Dur = [Math]::Round(((Get-Date) - $T0).TotalMinutes, 1)

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## parcelles.json -- parcelles cadastrales (verrou 1 des SOLS PAR LE CADASTRE)')
$L.Add("- Source : WFS Geoplateforme CADASTRALPARCELS.PARCELLAIRE_EXPRESS:parcelle (PCI Express), fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte (IGN/DGFiP).")
$L.Add("- Objets : $NbFeat features -> $NbPoly polygones emis ($NbMulti features multipolygones, $NbSkip sans polygone exploitable), $NbTrous anneaux interieurs conserves.")
$L.Add("- Taille : $SizeMo Mo. Duree : $Dur min.")
$L.Add("- Sections distinctes : $($Sections.Count).")
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $NbPoly polygones, $NbTrous trous, $SizeMo Mo, $Dur min"
