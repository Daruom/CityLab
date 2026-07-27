# GF-Common.ps1 -- conventions communes du GRAND FETCH Toulouse (a dot-sourcer).
# Reprend les patterns canoniques de Fetch-Toulouse10.ps1 / Fetch-Toulouse10-Surfaces.ps1 :
#  - projection locale equirectangulaire, origine place du Capitole, NORD = -Y
#    (chiralite Unreal main gauche, cf. Fetch-Toulouse10.ps1) ;
#  - WFS Geoplateforme pagine COUNT=1000 + pause 1200 ms (le serveur jette les
#    requetes enchainees), 6 retries / 5 s ;
#  - Overpass : 3 miroirs alternes, POST, 6 retries / 20 s, pause entre tuiles ;
#  - PIEGE UTF-8 PS 5.1 : Invoke-RestMethod peut decoder l'UTF-8 en ISO-8859-1
#    (mojibake constate sur le fetch markers historique) -> ici Invoke-WebRequest
#    + decodage explicite des bytes en UTF-8 ;
#  - JSON de sortie au StringBuilder (ConvertTo-Json trop lent), UTF-8 sans BOM.
$ErrorActionPreference = 'Stop'
$Inv = [System.Globalization.CultureInfo]::InvariantCulture
try { [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072 } catch { }

$GFDir = $PSScriptRoot
$GFLogPath = Join-Path $GFDir 'progress.log'
$GFRapportPath = Join-Path $GFDir 'GrandFetch-Rapport.md'
$GFUtf8 = New-Object System.Text.UTF8Encoding $false

$Lat0 = 43.6045; $Lon0 = 1.4442
$MPerLat = 110540.0
$MPerLon = 111320.0 * [Math]::Cos($Lat0 * [Math]::PI / 180)

function F([double]$V) { return $V.ToString('0.##', $Inv) }
function ToLocalX([double]$Lon) { return [Math]::Round(($Lon - $Lon0) * $MPerLon, 2) }
# NORD = -Y : Unreal est main GAUCHE (cf. Fetch-Toulouse10.ps1, constate en vol).
function ToLocalY([double]$Lat) { return [Math]::Round(($Lat0 - $Lat) * $MPerLat, 2) }

function GFBbox([double]$HalfM) {
	$DLat = $HalfM / $MPerLat; $DLon = $HalfM / $MPerLon
	return @{ S = $Lat0 - $DLat; N = $Lat0 + $DLat; W = $Lon0 - $DLon; E = $Lon0 + $DLon }
}

function GFAppendFile([string]$Path, [string]$Text) {
	for ($i = 0; $i -lt 10; $i++) {
		try { [System.IO.File]::AppendAllText($Path, $Text, $GFUtf8); return }
		catch { Start-Sleep -Milliseconds 300 }
	}
}

# Heartbeat surveille par le superviseur : APPEND a chaque page WFS / tuile Overpass.
function GFLog([string]$Theme, [string]$Detail) {
	$Line = '{0}  {1}  {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Theme, $Detail
	GFAppendFile $GFLogPath ($Line + "`r`n")
	Write-Host $Line
}

function GFRapportAdd([string]$Text) { GFAppendFile $GFRapportPath ($Text + "`r`n") }

# GET + decodage UTF-8 explicite + parse JSON (anti-mojibake PS 5.1).
function GFHttpJson([string]$Url) {
	$Resp = Invoke-WebRequest -Uri $Url -TimeoutSec 180 -UserAgent 'CityLab-DroneFPV/1.0' -UseBasicParsing
	$Txt = [System.Text.Encoding]::UTF8.GetString($Resp.RawContentStream.ToArray())
	return ($Txt | ConvertFrom-Json)
}

function GFWfsPage([string]$Layer, $B, [int]$Start, [string]$Theme) {
	# Ordre BBOX = W,S,E,N (lon,lat) : ordre VALIDE par Fetch-Toulouse10.ps1 sur ce serveur.
	$Url = "https://data.geopf.fr/wfs/ows?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=$Layer&SRSNAME=EPSG:4326&BBOX=$($B.W),$($B.S),$($B.E),$($B.N),EPSG:4326&OUTPUTFORMAT=application/json&COUNT=1000&STARTINDEX=$Start"
	for ($Try = 1; $Try -le 6; $Try++) {
		try { return (GFHttpJson $Url) }
		catch {
			if ($Try -eq 6) { throw }
			GFLog $Theme "WFS $Layer STARTINDEX=$Start echec ($Try/6) : $($_.Exception.Message) -- retry 5 s"
			Start-Sleep 5
		}
	}
}

# Toutes les pages d'une couche (petites et moyennes couches ; pour les tres
# grosses -- batiment -- boucler soi-meme page par page pour ne pas tout garder).
function GFWfsAll([string]$Layer, $B, [string]$Theme) {
	$Out = New-Object System.Collections.Generic.List[object]
	$Start = 0
	do {
		$R = GFWfsPage $Layer $B $Start $Theme
		$Feats = @($R.features)
		foreach ($Feat in $Feats) { $Out.Add($Feat) }
		$Got = $Feats.Count
		GFLog $Theme "WFS $Layer page STARTINDEX=$Start : $Got features (cumul $($Out.Count))"
		$Start += 1000
		Start-Sleep -Milliseconds 1200
	} while ($Got -eq 1000)
	return ,$Out
}

$GFMirrors = @('https://overpass.openstreetmap.fr/api/interpreter',
	'https://overpass.kumi.systems/api/interpreter',
	'https://overpass-api.de/api/interpreter')

function GFOverpass([string]$Query, [string]$Theme, [string]$Label) {
	$Body = 'data=' + [Uri]::EscapeDataString($Query)
	for ($Try = 0; $Try -lt 6; $Try++) {
		$Mirror = $GFMirrors[$Try % $GFMirrors.Count]
		try {
			$Resp = Invoke-WebRequest -Uri $Mirror -Method Post -Body $Body -ContentType 'application/x-www-form-urlencoded' -UserAgent 'CityLab-DroneFPV/1.0 (contact: mourradmohsen@gmail.com)' -TimeoutSec 300 -UseBasicParsing
			$O = [System.Text.Encoding]::UTF8.GetString($Resp.RawContentStream.ToArray()) | ConvertFrom-Json
			if ($O.remark -and "$($O.remark)" -match 'error') { throw "remark Overpass : $($O.remark)" }
			GFLog $Theme "Overpass $Label OK via $Mirror ($(@($O.elements).Count) elements)"
			return $O
		} catch {
			GFLog $Theme "Overpass $Label echec sur $Mirror : $($_.Exception.Message) -- retry 20 s"
			Start-Sleep 20
		}
	}
	throw "Overpass : $Label irrecuperable apres 6 essais."
}

function GFEsc([string]$S) {
	if ($null -eq $S) { return '' }
	$S = $S.Replace('\', '\\').Replace('"', '\"')
	return ($S -replace '[\x00-\x1f]', ' ')
}

# Anneaux exterieurs d'une geometrie Polygon / MultiPolygon (coordonnees 2D ou 3D).
function GFExteriorRings($Geom) {
	$Rings = New-Object System.Collections.Generic.List[object]
	if ($null -eq $Geom -or $null -eq $Geom.coordinates) { return ,$Rings }
	if ($Geom.type -eq 'Polygon') {
		if (@($Geom.coordinates).Count -ge 1) { $Rings.Add($Geom.coordinates[0]) }
	} elseif ($Geom.type -eq 'MultiPolygon') {
		foreach ($Poly in $Geom.coordinates) { if (@($Poly).Count -ge 1) { $Rings.Add($Poly[0]) } }
	}
	return ,$Rings
}

# Polygones AVEC leurs anneaux interieurs (les cours). Un polygone GeoJSON est un
# tableau d'anneaux : [0] = exterieur, [1..] = trous. GFExteriorRings jette les
# trous (bon pour les fetchs qui n'en veulent pas) ; ici on renvoie le tableau
# d'anneaux COMPLET de chaque polygone, pour que Fetch-GF-Cours.ps1 recupere les
# cours interieures. Ne remplace PAS GFExteriorRings.
function GFRingsWithHoles($Geom) {
	$Polys = New-Object System.Collections.Generic.List[object]
	if ($null -eq $Geom -or $null -eq $Geom.coordinates) { return ,$Polys }
	if ($Geom.type -eq 'Polygon') {
		if (@($Geom.coordinates).Count -ge 1) { $Polys.Add($Geom.coordinates) }
	} elseif ($Geom.type -eq 'MultiPolygon') {
		foreach ($Poly in $Geom.coordinates) { if (@($Poly).Count -ge 1) { $Polys.Add($Poly) } }
	}
	return ,$Polys
}

# Aire (m2, valeur absolue) d'un anneau [ [x,y], ... ] en coordonnees LOCALES.
# NB : $Pts.Count directement (PAS @($Pts).Count : sur une List[object] de tableaux,
# @() casse en ArgumentException sous PS 5.1).
function GFRingAreaM2($Pts) {
	if ($null -eq $Pts) { return 0.0 }
	$N = $Pts.Count
	if ($N -lt 3) { return 0.0 }
	$A = 0.0
	for ($i = 0; $i -lt $N; $i++) {
		$P = $Pts[$i]; $Q = $Pts[($i + 1) % $N]
		$A += ([double]$P[0]) * ([double]$Q[1]) - ([double]$Q[0]) * ([double]$P[1])
	}
	return [Math]::Abs($A) / 2.0
}

function GFLines($Geom) {
	$Lines = New-Object System.Collections.Generic.List[object]
	if ($null -eq $Geom -or $null -eq $Geom.coordinates) { return ,$Lines }
	if ($Geom.type -eq 'LineString') { $Lines.Add($Geom.coordinates) }
	elseif ($Geom.type -eq 'MultiLineString') { foreach ($L in $Geom.coordinates) { $Lines.Add($L) } }
	return ,$Lines
}

# Amincit un anneau [lon,lat(,z)] -> liste de [x,y] locaux. Plafond par DECIMATION
# (pattern ThinRing de Fetch-Toulouse10-Surfaces.ps1) : aucun objet perdu.
function GFThinRing($Ring, [double]$Seuil, [int]$Plafond) {
	$Pts = New-Object System.Collections.Generic.List[object]
	$Prev = $null
	foreach ($C in $Ring) {
		$P = @((ToLocalX $C[0]), (ToLocalY $C[1]))
		if ($Prev -and ([Math]::Abs($P[0]-$Prev[0]) + [Math]::Abs($P[1]-$Prev[1])) -lt $Seuil) { continue }
		$Pts.Add($P); $Prev = $P
	}
	if ($Pts.Count -gt 1) {
		$A = $Pts[0]; $Z = $Pts[$Pts.Count-1]
		if (([Math]::Abs($A[0]-$Z[0]) + [Math]::Abs($A[1]-$Z[1])) -lt $Seuil) { $Pts.RemoveAt($Pts.Count-1) }
	}
	if ($Plafond -gt 0 -and $Pts.Count -gt $Plafond) {
		$Step = [Math]::Ceiling($Pts.Count / [double]$Plafond)
		$Thin = New-Object System.Collections.Generic.List[object]
		for ($i = 0; $i -lt $Pts.Count; $i += $Step) { $Thin.Add($Pts[$i]) }
		$Pts = $Thin
	}
	return ,$Pts
}

# Amincit une polyligne en conservant le dernier point (connectivite des extremites).
function GFThinLine($Coords, [double]$Seuil) {
	$Pts = New-Object System.Collections.Generic.List[object]
	$Prev = $null
	foreach ($C in $Coords) {
		$P = @((ToLocalX $C[0]), (ToLocalY $C[1]))
		if ($Prev -and ([Math]::Abs($P[0]-$Prev[0]) + [Math]::Abs($P[1]-$Prev[1])) -lt $Seuil) { continue }
		$Pts.Add($P); $Prev = $P
	}
	$NC = @($Coords).Count
	if ($NC -ge 2 -and $Pts.Count -ge 1) {
		$Last = $Coords[$NC - 1]
		$PL = @((ToLocalX $Last[0]), (ToLocalY $Last[1]))
		$PE = $Pts[$Pts.Count - 1]
		if (([Math]::Abs($PL[0]-$PE[0]) + [Math]::Abs($PL[1]-$PE[1])) -gt 0.001) { $Pts.Add($PL) }
	}
	return ,$Pts
}

function GFAppendPts([System.Text.StringBuilder]$Sb, $Pts) {
	[void]$Sb.Append('[')
	for ($i = 0; $i -lt $Pts.Count; $i++) {
		if ($i -gt 0) { [void]$Sb.Append(',') }
		[void]$Sb.Append('[').Append((F $Pts[$i][0])).Append(',').Append((F $Pts[$i][1])).Append(']')
	}
	[void]$Sb.Append(']')
}

function GFNewBounds() { return @{ MinX = [double]::MaxValue; MaxX = [double]::MinValue; MinY = [double]::MaxValue; MaxY = [double]::MinValue; N = 0 } }
function GFBoundsPts($Bd, $Pts) {
	foreach ($P in $Pts) {
		if ($P[0] -lt $Bd.MinX) { $Bd.MinX = $P[0] }
		if ($P[0] -gt $Bd.MaxX) { $Bd.MaxX = $P[0] }
		if ($P[1] -lt $Bd.MinY) { $Bd.MinY = $P[1] }
		if ($P[1] -gt $Bd.MaxY) { $Bd.MaxY = $P[1] }
		$Bd.N++
	}
}
function GFBoundsText($Bd) {
	if ($Bd.N -eq 0) { return 'aucun point' }
	return ('X [' + (F $Bd.MinX) + ' ; ' + (F $Bd.MaxX) + '] m, Y [' + (F $Bd.MinY) + ' ; ' + (F $Bd.MaxY) + '] m (' + $Bd.N + ' pts)')
}

# Cles scalaires d'un feature WFS, moins le bruit technique (ids, dates, sources,
# precisions). L'acquisition garde le reste BRUT : les builders decideront.
$GFPropBlacklist = '^(cleabs|gml_id|gcms_.*|date_.*|source.*|identifiants_.*|precision_.*|methode_.*|liens_.*)$'
function GFScalarKeys($Props) {
	$Keys = @()
	if ($null -eq $Props) { return ,$Keys }
	foreach ($P in $Props.PSObject.Properties) {
		if ($P.Name -match $GFPropBlacklist) { continue }
		$V = $P.Value
		if ($null -ne $V -and ($V -is [System.Array] -or $V -is [System.Management.Automation.PSCustomObject])) { continue }
		$Keys += $P.Name
	}
	return ,$Keys
}

# Ajoute au StringBuilder les proprietes non nulles (valeurs BRUTES) et compte le
# remplissage par cle dans $Fill (la matiere premiere du rapport).
function GFAppendPropsCount([System.Text.StringBuilder]$Sb, $Props, [string[]]$Keys, $Fill) {
	foreach ($K in $Keys) {
		$V = $Props.$K
		if ($null -eq $V) { continue }
		if ($V -is [System.Array] -or $V -is [System.Management.Automation.PSCustomObject]) { continue }
		if ($V -is [bool]) {
			[void]$Sb.Append(',"').Append($K).Append('":').Append($(if ($V) { 'true' } else { 'false' }))
		} elseif ($V -is [int] -or $V -is [long] -or $V -is [double] -or $V -is [decimal] -or $V -is [single] -or $V -is [int16] -or $V -is [byte]) {
			[void]$Sb.Append(',"').Append($K).Append('":').Append(([double]$V).ToString('0.###', $Inv))
		} else {
			$S = "$V"
			if ($S -eq '') { continue }
			[void]$Sb.Append(',"').Append($K).Append('":"').Append((GFEsc $S)).Append('"')
		}
		if ($Fill.ContainsKey($K)) { $Fill[$K] = $Fill[$K] + 1 } else { $Fill[$K] = 1 }
	}
}

function GFFillText([string[]]$Keys, $Fill, [int]$Total) {
	$Lines = New-Object System.Collections.Generic.List[string]
	foreach ($K in $Keys) {
		$C = 0; if ($Fill.ContainsKey($K)) { $C = $Fill[$K] }
		$Pct = 0.0; if ($Total -gt 0) { $Pct = 100.0 * $C / $Total }
		$Lines.Add('  - ' + $K + ' : ' + $C + ' / ' + $Total + ' (' + $Pct.ToString('0.0', $Inv) + ' %)')
	}
	return ($Lines -join "`r`n")
}

function GFWriteJson([string]$Path, [string]$Text) {
	[System.IO.File]::WriteAllText($Path, $Text, $GFUtf8)
}
