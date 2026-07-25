# Fetch-GF-RuesNommees.ps1 -- GRAND FETCH theme 2 : rues nommees (OSM Overpass).
# way["highway"]["name"] sur la bbox standard, en 9 tuiles, dedup par id.
# Polyligne complete (pas d'amincissement, parite avec le fetch routes historique)
# + name + type highway + ref eventuel. Types proposed/construction ecartes.
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'rues_nommees'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'rues_nommees.json'

$Seen = New-Object 'System.Collections.Generic.HashSet[long]'
$Sb = New-Object System.Text.StringBuilder
$Bd = GFNewBounds
$Nb = 0; $NbRef = 0; $NbEcartes = 0
$Samples = New-Object System.Collections.Generic.List[string]
$SampleAccent = $null

GFLog $Theme 'DEBUT (Overpass way[highway][name], 9 tuiles)'
$Tiles = 3
for ($Ty = 0; $Ty -lt $Tiles; $Ty++) {
	for ($Tx = 0; $Tx -lt $Tiles; $Tx++) {
		$TS = $B.S + ($B.N - $B.S) * $Ty / $Tiles; $TN = $B.S + ($B.N - $B.S) * ($Ty + 1) / $Tiles
		$TW = $B.W + ($B.E - $B.W) * $Tx / $Tiles; $TE = $B.W + ($B.E - $B.W) * ($Tx + 1) / $Tiles
		$Q = @"
[out:json][timeout:180];
way["highway"]["name"]($TS,$TW,$TN,$TE);
out geom;
"@
		$O = GFOverpass $Q $Theme "tuile ($Tx,$Ty)"
		$TileN = 0
		foreach ($El in $O.elements) {
			if ($El.type -ne 'way' -or -not $El.geometry) { continue }
			if (-not $Seen.Add([long]$El.id)) { continue }
			$T = "$($El.tags.highway)"
			if ($T -in @('proposed', 'construction')) { $NbEcartes++; continue }
			$Name = "$($El.tags.name)"
			if ($Name -eq '') { continue }
			$Pts = New-Object System.Collections.Generic.List[object]
			foreach ($G in $El.geometry) { $Pts.Add(@((ToLocalX $G.lon), (ToLocalY $G.lat))) }
			if ($Pts.Count -lt 2) { continue }
			if ($Nb -gt 0) { [void]$Sb.Append(',') }
			[void]$Sb.Append('{"pts":')
			GFAppendPts $Sb $Pts
			[void]$Sb.Append(',"name":"').Append((GFEsc $Name)).Append('","t":"').Append((GFEsc $T)).Append('"')
			if ($El.tags.ref) {
				[void]$Sb.Append(',"ref":"').Append((GFEsc "$($El.tags.ref)")).Append('"')
				$NbRef++
			}
			[void]$Sb.Append('}')
			GFBoundsPts $Bd $Pts
			if ($Samples.Count -lt 2) { $Samples.Add("name=$Name t=$T pts=$($Pts.Count) X=$(F $Pts[0][0]) Y=$(F $Pts[0][1])") }
			if (-not $SampleAccent -and $Name -match '[^\x00-\x7f]') { $SampleAccent = "name=$Name t=$T ref=$($El.tags.ref) X=$(F $Pts[0][0]) Y=$(F $Pts[0][1])" }
			$Nb++; $TileN++
		}
		GFLog $Theme "tuile ($Tx,$Ty) : +$TileN rues (cumul $Nb)"
		Start-Sleep 2
	}
}

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"roads":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$PctRef = 0.0; if ($Nb -gt 0) { $PctRef = 100.0 * $NbRef / $Nb }
$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## rues_nommees.json -- rues nommees')
$L.Add("- Source : OpenStreetMap via Overpass (way[highway][name]), fetch du $(Get-Date -Format 'yyyy-MM-dd'), licence ODbL 1.0.")
$L.Add("- Objets : $Nb ways nommes ($NbEcartes proposed/construction ecartes).")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage :')
$L.Add('  - name : ' + $Nb + ' / ' + $Nb + ' (100.0 %) (filtre de la requete)')
$L.Add('  - ref : ' + $NbRef + ' / ' + $Nb + ' (' + $PctRef.ToString('0.0', $Inv) + ' %)')
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- Temoins :')
foreach ($S in $Samples) { $L.Add("  - $S") }
if ($SampleAccent) { $L.Add("  - (accent) $SampleAccent") }
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $Nb rues, $SizeMo Mo"
