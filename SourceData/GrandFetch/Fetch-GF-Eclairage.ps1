# Fetch-GF-Eclairage.ps1 -- GRAND FETCH theme 7 : eclairage public (OSM Overpass).
# node["highway"="street_lamp"] sur la bbox standard, 9 tuiles, dedup par id.
# Points seuls [x,y] (pas d'attributs, mission acquisition).
. (Join-Path $PSScriptRoot 'GF-Common.ps1')

$Theme = 'eclairage'
$B = GFBbox 5000.0
$OutPath = Join-Path $GFDir 'eclairage.json'

$Seen = New-Object 'System.Collections.Generic.HashSet[long]'
$Sb = New-Object System.Text.StringBuilder
$Bd = GFNewBounds
$Nb = 0

GFLog $Theme 'DEBUT (Overpass node[highway=street_lamp], 9 tuiles)'
$Tiles = 3
for ($Ty = 0; $Ty -lt $Tiles; $Ty++) {
	for ($Tx = 0; $Tx -lt $Tiles; $Tx++) {
		$TS = $B.S + ($B.N - $B.S) * $Ty / $Tiles; $TN = $B.S + ($B.N - $B.S) * ($Ty + 1) / $Tiles
		$TW = $B.W + ($B.E - $B.W) * $Tx / $Tiles; $TE = $B.W + ($B.E - $B.W) * ($Tx + 1) / $Tiles
		$Q = @"
[out:json][timeout:180];
node["highway"="street_lamp"]($TS,$TW,$TN,$TE);
out;
"@
		$O = GFOverpass $Q $Theme "tuile ($Tx,$Ty)"
		$TileN = 0
		foreach ($El in $O.elements) {
			if ($El.type -ne 'node') { continue }
			if (-not $Seen.Add([long]$El.id)) { continue }
			$P = @((ToLocalX $El.lon), (ToLocalY $El.lat))
			if ($Nb -gt 0) { [void]$Sb.Append(',') }
			[void]$Sb.Append('[').Append((F $P[0])).Append(',').Append((F $P[1])).Append(']')
			GFBoundsPts $Bd @(, $P)
			$Nb++; $TileN++
		}
		GFLog $Theme "tuile ($Tx,$Ty) : +$TileN lampadaires (cumul $Nb)"
		Start-Sleep 2
	}
}

$Out = New-Object System.Text.StringBuilder
[void]$Out.Append('{"origin":{"lat":43.6045,"lon":1.4442},"sizeM":{"x":10000,"y":10000},"lamps":[')
[void]$Out.Append($Sb.ToString()).Append(']}')
GFWriteJson $OutPath $Out.ToString()
$SizeMo = [Math]::Round((Get-Item $OutPath).Length / 1MB, 2)

$Densite = [Math]::Round($Nb / 100.0, 1)
$L = New-Object System.Collections.Generic.List[string]
$L.Add('')
$L.Add('## eclairage.json -- lampadaires')
$L.Add("- Source : OpenStreetMap via Overpass (node[highway=street_lamp]), fetch du $(Get-Date -Format 'yyyy-MM-dd'), licence ODbL 1.0.")
$L.Add("- Objets : $Nb lampadaires (points seuls, dedup par id). Densite moyenne $Densite / km2.")
$L.Add("- Taille : $SizeMo Mo.")
$L.Add("- Taux de remplissage : sans objet (points nus par design, aucun attribut conserve).")
$L.Add('- Bornes : ' + (GFBoundsText $Bd))
$L.Add('- Temoins : 3 premiers points du fichier (pas de champ nominal sur ce theme).')
$L.Add("- ATTENTION builders : la couverture street_lamp d'OSM est VOLONTAIRE (crowdsourcee), donc heterogene par quartier -- une densite faible dans un secteur ne veut pas dire absence de lampadaires reels.")
GFRapportAdd (($L -join "`r`n"))
GFLog $Theme "FIN : $Nb lampadaires, $SizeMo Mo"
