# J3c - recupere la BD ORTHO (photo aerienne IGN 20 cm/px, Licence Ouverte 2.0) en
# tuiles alignees sur les CELLULES 500 m de la generation (meme frame locale que
# toulouse10*.json : origine Capitole, nord = -Y, equirectangulaire).
# Une cellule (cx,cy) couvre x [cx*500,(cx+1)*500], y [cy*500,(cy+1)*500] m locaux
# -> tuile 2500x2500 px (20 cm/px) nommee Ortho/ortho_20cm_<cx>_<cy>.jpg.
# Usage : .\Fetch-Toulouse10-Ortho.ps1 -Cells "-1,-1 0,-1 -1,0 0,0"
#         .\Fetch-Toulouse10-Ortho.ps1 -All   # dalle entiere 20x20 (~600 Mo, ~7 min)
param(
	[string]$Cells = "-1,-1 0,-1 -1,0 0,0",
	[switch]$All
)
$ErrorActionPreference = 'Stop'
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

$Lat0 = 43.6045; $Lon0 = 1.4442
$MPerLat = 110540.0
$MPerLon = 111320.0 * [Math]::Cos($Lat0 * [Math]::PI / 180)
$CellM = 500.0
$Px = 2500

$OutDir = Join-Path $PSScriptRoot 'Ortho'
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }

$List = @()
if ($All) {
	for ($cy = -10; $cy -lt 10; $cy++) { for ($cx = -10; $cx -lt 10; $cx++) { $List += , @($cx, $cy) } }
} else {
	foreach ($Tok in $Cells -split ' ') {
		$P = $Tok -split ','
		$List += , @([int]$P[0], [int]$P[1])
	}
}

Write-Host "BD ORTHO : $($List.Count) tuiles de $Px px (20 cm/px)..."
$N = 0
foreach ($C in $List) {
	$Cx = $C[0]; $Cy = $C[1]
	$Out = Join-Path $OutDir "ortho_20cm_${Cx}_${Cy}.jpg"
	if (Test-Path $Out) { $N++; continue }
	# Frame locale -> WGS84. y local croit vers le SUD (nord = -Y).
	$X0 = $Cx * $CellM; $X1 = ($Cx + 1) * $CellM
	$Y0 = $Cy * $CellM; $Y1 = ($Cy + 1) * $CellM
	$W = $Lon0 + $X0 / $MPerLon; $E = $Lon0 + $X1 / $MPerLon
	$NLat = $Lat0 - $Y0 / $MPerLat; $S = $Lat0 - $Y1 / $MPerLat
	# WMS 1.3.0 + EPSG:4326 : ordre lat,lon dans BBOX.
	$Bbox = ('{0},{1},{2},{3}' -f $S.ToString('0.########', $Inv), $W.ToString('0.########', $Inv),
		$NLat.ToString('0.########', $Inv), $E.ToString('0.########', $Inv))
	$Url = "https://data.geopf.fr/wms-r/wms?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap" +
		"&LAYERS=HR.ORTHOIMAGERY.ORTHOPHOTOS&STYLES=&CRS=EPSG:4326&BBOX=$Bbox" +
		"&WIDTH=$Px&HEIGHT=$Px&FORMAT=image/jpeg"
	for ($Try = 1; $Try -le 6; $Try++) {
		try {
			Invoke-WebRequest -Uri $Url -OutFile $Out -TimeoutSec 120 -UserAgent 'CityLab-DroneFPV/1.0'
			break
		} catch {
			if ($Try -eq 6) { throw }
			Write-Host "  tuile ($Cx,$Cy) : echec ($Try), retry 5 s"
			Start-Sleep 5
		}
	}
	# Une reponse d'erreur WMS est du XML : la detecter (JPEG commence par FF D8).
	$Head = [System.IO.File]::ReadAllBytes($Out)[0..1]
	if ($Head[0] -ne 0xFF -or $Head[1] -ne 0xD8) {
		$Txt = Get-Content $Out -Raw
		Remove-Item $Out
		throw "Tuile ($Cx,$Cy) : reponse non-JPEG : $($Txt.Substring(0, [Math]::Min(200, $Txt.Length)))"
	}
	$N++
	if (($N % 20) -eq 0) { Write-Host "  $N/$($List.Count)" }
	Start-Sleep -Milliseconds 600
}
$Files = Get-ChildItem $OutDir -Filter 'ortho_20cm_*.jpg'
$Mo = [Math]::Round(($Files | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "FIN : $($Files.Count) tuiles sur disque ($Mo Mo) dans $OutDir"
Write-Host "Georef : tuile (cx,cy) = x [cx*500,(cx+1)*500] m, y [cy*500,(cy+1)*500] m locaux ;"
Write-Host "pixel (0,0) = coin NW de la tuile ; 1 px = 20 cm. Source IGN BD ORTHO, Licence Ouverte 2.0."