# Fetch-GF-EchantillonsJ5.ps1 -- GRAND FETCH theme 12 : echantillons J5.
# But = VALIDER LE FORMAT pour J5, pas exploiter maintenant. Bbox standard : le
# 10x10 urbain donne peu d'objets agricoles/forestiers, c'est OK (franges).
#  - rpg.json   : RPG.LATEST:parcelles_graphiques (code culture)
#  - foret.json : LANDCOVER.FORESTINVENTORY.V2:formation_vegetale (BD Foret v2)
#  - haie.json  : BDTOPO_V3:haie
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'echantillons_j5'
$B = GFBbox 5000.0
$OutDir = Join-Path $GFDir 'echantillons_j5'
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
GFLog $Theme 'DEBUT (RPG + BD Foret + haies, bbox standard)'

$Resultats = New-Object System.Collections.Generic.List[string]

# Fetch generique : surfaces ('rings') ou lignes ('pts') selon la geometrie.
function FetchEchantillon([string]$Layer, [string]$OutFile, [string]$ArrayName, [double]$Seuil) {
	$OutPath = Join-Path $OutDir $OutFile
	try {
		$Sb = New-Object System.Text.StringBuilder
		$Keys = $null; $Fill = @{}; $Nb = 0
		$Bd = GFNewBounds
		$Temoin = $null; $TemoinAccent = $null
		foreach ($Feat in (GFWfsAll $Layer $B $Theme)) {
			$Props = $Feat.properties
			if ($null -eq $Keys) {
				$Keys = GFScalarKeys $Props
				GFLog $Theme ("attributs " + $OutFile + " : " + ($Keys -join ', '))
			}
			$Emis = $false
			$Rings = GFExteriorRings $Feat.geometry
			if ($Rings.Count -ge 1) {
				$SbR = New-Object System.Text.StringBuilder
				$NbR = 0
				foreach ($Ring in $Rings) {
					$Pts = GFThinRing $Ring $Seuil 400
					if ($Pts.Count -lt 3) { continue }
					if ($NbR -gt 0) { [void]$SbR.Append(',') }
					GFAppendPts $SbR $Pts
					GFBoundsPts $Bd $Pts
					$NbR++
				}
				if ($NbR -ge 1) {
					if ($Nb -gt 0) { [void]$Sb.Append(',') }
					[void]$Sb.Append('{"rings":[').Append($SbR.ToString()).Append(']')
					GFAppendPropsCount $Sb $Props $Keys $Fill
					[void]$Sb.Append('}')
					$Nb++; $Emis = $true
				}
			}
			if (-not $Emis) {
				foreach ($Line in (GFLines $Feat.geometry)) {
					$Pts = GFThinLine $Line $Seuil
					if ($Pts.Count -lt 2) { continue }
					if ($Nb -gt 0) { [void]$Sb.Append(',') }
					[void]$Sb.Append('{"pts":')
					GFAppendPts $Sb $Pts
					GFAppendPropsCount $Sb $Props $Keys $Fill
					[void]$Sb.Append('}')
					GFBoundsPts $Bd $Pts
					$Nb++; $Emis = $true
				}
			}
			if ($Emis -and $null -eq $Temoin) {
				$Vals = New-Object System.Collections.Generic.List[string]
				foreach ($K in $Keys) { $V = $Props.$K; if ($null -ne $V -and "$V" -ne '') { $Vals.Add("$K=$V") } }
				$Temoin = ($Vals -join ' ')
			}
			if ($Emis -and $null -eq $TemoinAccent) {
				foreach ($K in $Keys) {
					$V = "$($Props.$K)"
					if ($V -match '[^\x00-\x7f]') { $TemoinAccent = "$K=$V"; break }
				}
			}
		}
		$Out = New-Object System.Text.StringBuilder
		[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"source":"')
		[void]$Out.Append($Layer).Append('","').Append($ArrayName).Append('":[').Append($Sb.ToString()).Append(']}')
		GFWriteJson $OutPath $Out.ToString()
		$SizeKo = [Math]::Round((Get-Item $OutPath).Length / 1KB, 0)
		$script:Resultats.Add("- $OutFile : $Nb objets ($SizeKo Ko), source $Layer.")
		if ($Keys -and $Nb -gt 0) {
			$script:Resultats.Add('  Remplissage :')
			$script:Resultats.Add((GFFillText $Keys $Fill $Nb))
			$script:Resultats.Add('  Bornes : ' + (GFBoundsText $Bd))
		}
		if ($Temoin) { $script:Resultats.Add("  Temoin : $Temoin") }
		if ($TemoinAccent) { $script:Resultats.Add("  Temoin accent : $TemoinAccent") }
		GFLog $Theme "$OutFile : $Nb objets"
	} catch {
		$script:Resultats.Add("- $OutFile : ECHEC ($Layer) : $($_.Exception.Message)")
		GFLog $Theme "$OutFile ECHEC : $($_.Exception.Message)"
	}
}

FetchEchantillon 'RPG.LATEST:parcelles_graphiques' 'rpg.json' 'parcelles' 2.0
FetchEchantillon 'LANDCOVER.FORESTINVENTORY.V2:formation_vegetale' 'foret.json' 'formations' 8.0
FetchEchantillon 'BDTOPO_V3:haie' 'haie.json' 'haies' 2.0

$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## echantillons_j5/ -- echantillons de format pour J5 (RPG, BD Foret, haies)')
$L.Add("- Sources : WFS Geoplateforme, fetch du $(Get-Date -Format 'yyyy-MM-dd'), Licence Ouverte 2.0 (IGN).")
$L.Add('- BUT : valider le format des couches pour J5, PAS une exploitation immediate.')
$L.Add('- Bbox standard 10x10 urbaine : peu d objets agricoles/forestiers attendus, les franges suffisent (assume).')
foreach ($S in $Resultats) { $L.Add($S) }
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme 'FIN'
