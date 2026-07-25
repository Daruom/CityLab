# Fetch-GF-Tunnels.ps1 -- GRAND FETCH theme 5 : tunnels et passages couverts (OSM).
# Exact complement du fetch routes historique (qui filtrait tunnel!=yes et
# covered!=yes) : way[highway] avec tunnel=yes OU covered=yes, PLUS way[railway]
# avec tunnel=yes (metro toulousain lignes A/B souterraines). Une seule requete
# bbox standard (volume modere). Conserve : pts, cat (highway|railway), t (valeur
# du tag), layer, name eventuel.
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'tunnels'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'tunnels.json'

$Seen = New-Object 'System.Collections.Generic.HashSet[long]'
$Sb = New-Object System.Text.StringBuilder
$Bd = GFNewBounds
$Nb = 0; $NbRail = 0; $NbMetro = 0; $NbLayer = 0; $NbName = 0
$Samples = New-Object System.Collections.Generic.List[string]
$SampleAccent = $null

GFLog $Theme 'DEBUT (Overpass tunnels highway+railway, 1 requete)'
$Q = @"
[out:json][timeout:180];
(
  way["highway"]["tunnel"="yes"]($($B.S),$($B.W),$($B.N),$($B.E));
  way["highway"]["covered"="yes"]($($B.S),$($B.W),$($B.N),$($B.E));
  way["railway"]["tunnel"="yes"]($($B.S),$($B.W),$($B.N),$($B.E));
);
out geom;
"@
$O = GFOverpass $Q $Theme 'bbox complete'
foreach ($El in $O.elements) {
	if ($El.type -ne 'way' -or -not $El.geometry) { continue }
	if (-not $Seen.Add([long]$El.id)) { continue }
	$Cat = 'highway'; $T = "$($El.tags.highway)"
	if ($El.tags.railway) { $Cat = 'railway'; $T = "$($El.tags.railway)"; $NbRail++ }
	if ($T -eq 'subway') { $NbMetro++ }
	$Layer = 0
	if ($El.tags.layer) {
		$LayerTmp = 0
		if ([int]::TryParse("$($El.tags.layer)", [ref]$LayerTmp)) { $Layer = $LayerTmp }
	}
	$Pts = New-Object System.Collections.Generic.List[object]
	foreach ($G in $El.geometry) { $Pts.Add(@((ToLocalX $G.lon), (ToLocalY $G.lat))) }
	if ($Pts.Count -lt 2) { continue }
	if ($Nb -gt 0) { [void]$Sb.Append(',') }
	[void]$Sb.Append('{"pts":')
	GFAppendPts $Sb $Pts
	[void]$Sb.Append(',"cat":"').Append($Cat).Append('","t":"').Append((GFEsc $T)).Append('"')
	[void]$Sb.Append(',"layer":').Append($Layer.ToString($Inv))
	if ($Layer -ne 0) { $NbLayer++ }
	if ($El.tags.name) {
		$Name = "$($El.tags.name)"
		[void]$Sb.Append(',"name":"').Append((GFEsc $Name)).Append('"')
		$NbName++
		if ($Samples.Count -lt 2) { $Samples.Add("cat=$Cat t=$T layer=$Layer name=$Name pts=$($Pts.Count)") }
		if (-not $SampleAccent -and $Name -match '[^\x00-\x7f]') { $SampleAccent = "cat=$Cat t=$T layer=$Layer name=$Name" }
	}
	[void]$Sb.Append('}')
	GFBoundsPts $Bd $Pts
	$Nb++
}

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"tunnels":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$PctLayer = 0.0; if ($Nb -gt 0) { $PctLayer = 100.0 * $NbLayer / $Nb }
$PctName = 0.0; if ($Nb -gt 0) { $PctName = 100.0 * $NbName / $Nb }
$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## tunnels.json -- tunnels et passages couverts')
$L.Add("- Source : OpenStreetMap via Overpass (way highway tunnel/covered=yes + way railway tunnel=yes), fetch du $(Get-Date -Format 'yyyy-MM-dd'), licence ODbL 1.0.")
$L.Add("- Objets : $Nb ways dont $NbRail ferroviaires ($NbMetro tags subway -- metro lignes A/B).")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add('- Taux de remplissage :')
$L.Add('  - layer != 0 : ' + $NbLayer + ' / ' + $Nb + ' (' + $PctLayer.ToString('0.0', $Inv) + ' %)')
$L.Add('  - name : ' + $NbName + ' / ' + $Nb + ' (' + $PctName.ToString('0.0', $Inv) + ' %)')
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- Temoins :')
foreach ($S in $Samples) { $L.Add("  - $S") }
if ($SampleAccent) { $L.Add("  - (accent) $SampleAccent") }
$L.Add("- Ces ways sont l'exact complement du filtre du fetch routes historique (toulouse10.json ne les contient pas).")
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $Nb tunnels, $SizeMo Mo"
