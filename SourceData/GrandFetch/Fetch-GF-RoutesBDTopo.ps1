# Fetch-GF-RoutesBDTopo.ps1 -- SOLS PAR LE CADASTRE, verrou 2 : axes routiers MESURES.
# BDTOPO_V3:troncon_de_route sur la dalle +-5000 m : polylignes (amincies 1 m,
# extremites conservees) + attributs BRUTS. La donnee critique est
# largeur_de_chaussee (metres) : elle remplace nos largeurs devinees.
# Rapport de verrou : taux de remplissage largeur / nombre_de_voies PAR importance.
# -HalfM : demi-cote de la dalle (5000 = production ; petite valeur = smoke test).
param([double]$HalfM = 5000.0, [string]$Suffixe = '')
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'routes_bdtopo'
$GFLogPath = Join-Path $GFDir "routes_bdtopo$Suffixe.progress.log"
$B = GFBbox $HalfM
$OutPath = Join-Path $GFDir "routes_bdtopo$Suffixe.json"
if ($Suffixe -ne '') { $GFRapportPath = Join-Path $GFDir "smoke_routes$Suffixe.md" }
$Bd = GFNewBounds
$T0 = Get-Date
GFLog $Theme "DEBUT (BDTOPO_V3:troncon_de_route, bbox +-$($HalfM.ToString('0', $Inv)) m)"

# Attributs retenus : tout ce qui sert a decider une largeur / un statut de voie.
$Keys = @('nature','importance','largeur_de_chaussee','nombre_de_voies','sens_de_circulation',
	'position_par_rapport_au_sol','fictif','prive','urbain','etat_de_l_objet',
	'acces_vehicule_leger','acces_pieton','reserve_aux_bus','vitesse_moyenne_vl',
	'cpx_classement_administratif','cpx_numero','nom_voie_ban_gauche','nom_collaboratif_gauche')

$Sb = New-Object System.Text.StringBuilder
$Fill = @{}
$NbFeat = 0; $NbLine = 0; $NbSkip = 0
$ByImp = @{}      # importance -> compteurs
$ByNat = @{}      # nature -> compteurs
$SumLargeur = 0.0; $NbLargeur = 0; $NbLargeurZero = 0
$Start = 0
do {
	$R = GFWfsPage 'BDTOPO_V3:troncon_de_route' $B $Start $Theme
	$Feats = @($R.features)
	$Got = $Feats.Count
	foreach ($Feat in $Feats) {
		$NbFeat++
		$Props = $Feat.properties
		$Emis = 0
		foreach ($Line in (GFLines $Feat.geometry)) {
			$Pts = GFThinLine $Line 1.0
			if ($Pts.Count -lt 2) { continue }
			if ($NbLine -gt 0) { [void]$Sb.Append(',') }
			[void]$Sb.Append('{"pts":')
			GFAppendPts $Sb $Pts
			GFAppendPropsCount $Sb $Props $Keys $Fill
			[void]$Sb.Append('}')
			GFBoundsPts $Bd $Pts
			$NbLine++; $Emis++
		}
		if ($Emis -eq 0) { $NbSkip++; continue }

		# --- statistiques du verrou (comptees par FEATURE, pas par polyligne) ---
		$Imp = "$($Props.importance)"; if ($Imp -eq '') { $Imp = '(vide)' }
		$Nat = "$($Props.nature)"; if ($Nat -eq '') { $Nat = '(vide)' }
		$HasL = 0; $HasV = 0
		$LV = $Props.largeur_de_chaussee
		if ($null -ne $LV -and "$LV" -ne '') {
			$LD = 0.0
			if ([double]::TryParse(("$LV" -replace ',', '.'), [System.Globalization.NumberStyles]::Float, $Inv, [ref]$LD)) {
				if ($LD -gt 0) { $HasL = 1; $NbLargeur++; $SumLargeur += $LD } else { $NbLargeurZero++ }
			}
		}
		$VV = $Props.nombre_de_voies
		if ($null -ne $VV -and "$VV" -ne '') {
			$VD = 0.0
			if ([double]::TryParse(("$VV" -replace ',', '.'), [System.Globalization.NumberStyles]::Float, $Inv, [ref]$VD)) {
				if ($VD -gt 0) { $HasV = 1 }
			}
		}
		foreach ($Pair in @(@($ByImp, $Imp), @($ByNat, $Nat))) {
			$Tbl = $Pair[0]; $K = $Pair[1]
			if (-not $Tbl.ContainsKey($K)) { $Tbl[$K] = @{ N = 0; L = 0; V = 0; SumL = 0.0 } }
			$E = $Tbl[$K]; $E.N++; $E.L += $HasL; $E.V += $HasV
			if ($HasL -eq 1) { $E.SumL += [double](("$LV" -replace ',', '.')) }
		}
	}
	GFLog $Theme "page STARTINDEX=$Start : $Got features (cumul feats $NbFeat, lignes $NbLine, largeur remplie $NbLargeur)"
	$Start += 1000
	Start-Sleep -Milliseconds 1200
} while ($Got -eq 1000)

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"source":"WFS Geoplateforme BDTOPO_V3:troncon_de_route","origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"troncons":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)
$Dur = [Math]::Round(((Get-Date) - $T0).TotalMinutes, 1)
$PctL = 0.0; if ($NbFeat -gt 0) { $PctL = 100.0 * $NbLargeur / $NbFeat }
$MoyL = 0.0; if ($NbLargeur -gt 0) { $MoyL = $SumLargeur / $NbLargeur }

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## routes_bdtopo.json -- troncons de route BD TOPO (verrou 2 : largeurs MESUREES)')
$L.Add("- Source : WFS Geoplateforme BDTOPO_V3:troncon_de_route, fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte 2.0 (IGN).")
$L.Add("- Objets : $NbFeat features -> $NbLine polylignes ($NbSkip sans polyligne exploitable).")
$L.Add("- Taille : $SizeMo Mo. Duree : $Dur min.")
$L.Add("- VERROU largeur_de_chaussee : $NbLargeur / $NbFeat features renseignes ($($PctL.ToString('0.0', $Inv)) %), moyenne $($MoyL.ToString('0.00', $Inv)) m, $NbLargeurZero valeurs a zero.")
$L.Add('- Taux de remplissage brut par attribut :')
$L.Add((GFFillText $Keys $Fill $NbLine))
$L.Add('- Croisement par importance (N features / largeur % / nb_voies % / largeur moyenne m) :')
foreach ($K in ($ByImp.Keys | Sort-Object)) {
	$E = $ByImp[$K]
	$PL = 100.0 * $E.L / $E.N; $PV = 100.0 * $E.V / $E.N
	$M = 0.0; if ($E.L -gt 0) { $M = $E.SumL / $E.L }
	$L.Add("  - importance $K : $($E.N) / $($PL.ToString('0.0', $Inv)) % / $($PV.ToString('0.0', $Inv)) % / $($M.ToString('0.00', $Inv)) m")
}
$L.Add('- Croisement par nature :')
foreach ($K in ($ByNat.Keys | Sort-Object)) {
	$E = $ByNat[$K]
	$PL = 100.0 * $E.L / $E.N; $PV = 100.0 * $E.V / $E.N
	$M = 0.0; if ($E.L -gt 0) { $M = $E.SumL / $E.L }
	$L.Add("  - $K : $($E.N) / $($PL.ToString('0.0', $Inv)) % / $($PV.ToString('0.0', $Inv)) % / $($M.ToString('0.00', $Inv)) m")
}
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $NbLine polylignes, largeur remplie $($PctL.ToString('0.0', $Inv)) %, $SizeMo Mo, $Dur min"
