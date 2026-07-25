# Recupere le MNT (modele numerique de terrain) IGN RGE ALTI 1 m sur le carre 10 x 10 km
# de toulouse10.json (meme origine place du Capitole, meme projection locale que
# Fetch-Toulouse10.ps1), puis assemble UNE heightmap exploitable par le pipeline :
#   SourceData\MNT\mnt_1m_<tx>_<ty>.bil : 25 tuiles 2 x 2 km, float32 LE brut (BIL 1 bande,
#       row-major, ligne 0 = nord), 2000 x 2000 px a 1 m/px = 16 000 000 octets exactement
#       (la taille exacte sert de controle d'integrite + reprise : tuile complete = skippee).
#   SourceData\toulouse10_mnt.png  : heightmap assemblee 10000 x 10000, PNG 16 bits gris,
#       valeur = altitude en CENTIMETRES (= Z Unreal en cm directement), filtre PNG "Up".
#   SourceData\toulouse10_mnt.json : georeferencement exact + stats (voir champs en fin).
#
# Source : WMS-Raster Geoplateforme (data.geopf.fr/wms-r), couche
#   ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES = "Modele Numerique de Terrain issu du RGEALTI"
#   (MNT sol nu, 1 m, Licence Ouverte 2.0 — attribution IGN). PAS de WCS sur data.geopf.fr
#   (endpoint inexistant, verifie 2026-07-25) ; le WMS-R sert le float32 natif via
#   FORMAT=image/x-bil;bits=32 (teste : Capitole 141-144 m). Limite serveur : 5010 px max.
# Pieges connus : la Geoplateforme jette les requetes enchainees sans pause -> 1,2 s entre
#   tuiles ; binaire -> Invoke-WebRequest -OutFile OBLIGATOIRE (jamais Invoke-RestMethod ni >).
#
# Usage :  .\Fetch-Toulouse10-MNT.ps1                (fetch avec reprise, puis assemblage)
#          .\Fetch-Toulouse10-MNT.ps1 -FetchOnly     (tuiles seulement)
#          .\Fetch-Toulouse10-MNT.ps1 -AssembleOnly  (assemblage seulement, tuiles deja la)
param([switch]$FetchOnly, [switch]$AssembleOnly)
$ErrorActionPreference = 'Stop'
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

# --- Projection locale : IDENTIQUE a Fetch-Toulouse10.ps1 (ne pas faire diverger) ---
$Lat0 = 43.6045; $Lon0 = 1.4442
$MPerLat = 110540.0
$MPerLon = 111320.0 * [Math]::Cos($Lat0 * [Math]::PI / 180)
$HalfM = 5000.0
$DLat = $HalfM / $MPerLat; $DLon = $HalfM / $MPerLon
$S = $Lat0 - $DLat; $N = $Lat0 + $DLat; $W = $Lon0 - $DLon; $E = $Lon0 + $DLon
# X = est = (lon-lon0)*MPerLon ; Y = (lat0-lat)*MPerLat -> NORD = -Y (Unreal main gauche,
# convention Cesium). La grille MNT est demandee en EPSG:4326 sur exactement [W..E]x[S..N]
# avec 1 px = 1 m PAR CONSTRUCTION dans cette projection locale (10000 px pour 10000 m).

$Tiles = 5          # grille 5 x 5 tuiles (comme le fetch Overpass)
$TilePx = 2000      # 2000 px = 2 km a 1 m/px ; < 5010 (limite WMS) ; 16 Mo/tuile
$SizePx = $Tiles * $TilePx
$TileBytes = $TilePx * $TilePx * 4
$OutDir = Join-Path $PSScriptRoot 'MNT'
$LatStep = ($N - $S) / $Tiles; $LonStep = ($E - $W) / $Tiles

# ---------- 1) Fetch des tuiles (reprise : tuile a la taille exacte = skippee) ----------
if (-not $AssembleOnly) {
	if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
	Write-Host "MNT RGE ALTI 1 m : $Tiles x $Tiles tuiles de $TilePx px ($([Math]::Round($TileBytes/1MB,1)) Mo/tuile)..."
	$NbNew = 0; $NbSkip = 0
	for ($Ty = 0; $Ty -lt $Tiles; $Ty++) {        # Ty 0 = rangee NORD (= ligne 0 de l'image)
		for ($Tx = 0; $Tx -lt $Tiles; $Tx++) {    # Tx 0 = colonne OUEST
			$Path = Join-Path $OutDir "mnt_1m_${Tx}_${Ty}.bil"
			if ((Test-Path $Path) -and (Get-Item $Path).Length -eq $TileBytes) { $NbSkip++; continue }
			$TLatN = $N - $Ty * $LatStep; $TLatS = $N - ($Ty + 1) * $LatStep
			$TLonW = $W + $Tx * $LonStep; $TLonE = $W + ($Tx + 1) * $LonStep
			# WMS 1.3.0 + EPSG:4326 -> ordre des axes BBOX = lat,lon (minLat,minLon,maxLat,maxLon)
			$Url = "https://data.geopf.fr/wms-r/wms?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap" +
				"&LAYERS=ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES&STYLES=&CRS=EPSG:4326" +
				"&BBOX=$TLatS,$TLonW,$TLatN,$TLonE&WIDTH=$TilePx&HEIGHT=$TilePx" +
				"&FORMAT=image/x-bil;bits=32"
			$Ok = $false
			for ($Try = 1; $Try -le 6; $Try++) {
				try {
					Invoke-WebRequest -Uri $Url -OutFile $Path -TimeoutSec 300 -UserAgent 'CityLab-DroneFPV/1.0'
					$Len = (Get-Item $Path).Length
					if ($Len -eq $TileBytes) { $Ok = $true; break }
					# Taille inattendue = reponse d'erreur (souvent XML) : diagnostiquer et refaire
					$Head = ''
					try { $Head = ([System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($Path), 0, [Math]::Min(200, $Len))) -replace '\s+', ' ' } catch {}
					Remove-Item $Path -Force
					Write-Host "  tuile ($Tx,$Ty) : $Len octets au lieu de $TileBytes ($Try/6) [$Head], retry dans 5 s"
					Start-Sleep 5
				} catch {
					if (Test-Path $Path) { Remove-Item $Path -Force }
					Write-Host "  tuile ($Tx,$Ty) : echec ($Try/6) [$($_.Exception.Message)], retry dans 5 s"
					Start-Sleep 5
				}
			}
			if (-not $Ok) { throw "MNT : tuile ($Tx,$Ty) irrecuperable apres 6 essais." }
			$NbNew++
			Write-Host "  tuile ($Tx,$Ty) OK ($($NbNew + $NbSkip)/$($Tiles*$Tiles))"
			Start-Sleep -Milliseconds 1200
		}
	}
	Write-Host "Tuiles : $NbNew telechargees, $NbSkip deja presentes."
	if ($FetchOnly) { return }
}

# ---------- 2) Assemblage -> PNG 16 bits (altitude en cm) + JSON de metadonnees ----------
# C# compile a la volee : boucles sur 10^8 px + deflate + CRC32/Adler32 (PowerShell pur = heures).
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.IO.Compression;

public static class MntPng
{
	static uint[] CrcTable;
	static uint CrcUpd(uint c, byte[] b, int off, int len)
	{
		if (CrcTable == null)
		{
			CrcTable = new uint[256];
			for (uint n = 0; n < 256; n++)
			{
				uint v = n;
				for (int k = 0; k < 8; k++) v = ((v & 1) != 0) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
				CrcTable[n] = v;
			}
		}
		for (int i = 0; i < len; i++) c = CrcTable[(c ^ b[off + i]) & 0xFF] ^ (c >> 8);
		return c;
	}
	static byte[] BE(uint v) { return new byte[] { (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v }; }
	static void Chunk(Stream s, string type, byte[] data)
	{
		byte[] t = System.Text.Encoding.ASCII.GetBytes(type);
		s.Write(BE((uint)data.Length), 0, 4);
		s.Write(t, 0, 4);
		s.Write(data, 0, data.Length);
		uint crc = CrcUpd(CrcUpd(0xFFFFFFFFu, t, 0, 4), data, 0, data.Length) ^ 0xFFFFFFFFu;
		s.Write(BE(crc), 0, 4);
	}

	// Assemble les tuiles BIL float32 en PNG 16 bits gris (valeur = altitude en cm, filtre Up).
	// Retourne { minAltM, maxAltM, moyAltM, nbNodata, altCapitoleM } (nodata < -1000 remplace par min).
	public static double[] Assemble(string tileDir, string pngPath, int tiles, int tilePx)
	{
		int size = tiles * tilePx;
		float[] alt = new float[(long)size * size];
		for (int ty = 0; ty < tiles; ty++)
			for (int tx = 0; tx < tiles; tx++)
			{
				string p = Path.Combine(tileDir, "mnt_1m_" + tx + "_" + ty + ".bil");
				byte[] b = File.ReadAllBytes(p);
				if (b.Length != tilePx * tilePx * 4) throw new Exception("Tuile invalide : " + p);
				for (int r = 0; r < tilePx; r++)
					Buffer.BlockCopy(b, r * tilePx * 4, alt, (int)(((long)(ty * tilePx + r) * size + tx * tilePx) * 4), tilePx * 4);
			}

		long n = (long)size * size, nod = 0;
		double min = double.MaxValue, max = double.MinValue, sum = 0;
		for (long i = 0; i < n; i++)
		{
			float v = alt[i];
			if (float.IsNaN(v) || v < -1000f) { nod++; continue; }
			if (v < min) min = v;
			if (v > max) max = v;
			sum += v;
		}
		if (nod == n) throw new Exception("Aucune valeur valide.");
		float rep = (float)min;
		double capitole = alt[(long)(size / 2) * size + size / 2];

		MemoryStream comp = new MemoryStream();
		comp.WriteByte(0x78); comp.WriteByte(0xDA);          // en-tete zlib (deflate, niveau max)
		uint aA = 1, aB = 0;                                  // Adler32 du flux filtre
		byte[] prev = new byte[size * 2], cur = new byte[size * 2], filt = new byte[size * 2 + 1];
		DeflateStream ds = new DeflateStream(comp, CompressionLevel.Optimal, true);
		for (int r = 0; r < size; r++)
		{
			long rowBase = (long)r * size;
			for (int c = 0; c < size; c++)
			{
				float v = alt[rowBase + c];
				if (float.IsNaN(v) || v < -1000f) v = rep;
				int cm = (int)Math.Round(v * 100.0);          // altitude en CENTIMETRES = Z Unreal
				if (cm < 0) cm = 0; if (cm > 65535) cm = 65535;
				cur[c * 2] = (byte)(cm >> 8); cur[c * 2 + 1] = (byte)cm;   // PNG = big-endian
			}
			filt[0] = 2;                                      // filtre "Up"
			for (int i = 0; i < size * 2; i++) filt[i + 1] = (byte)(cur[i] - prev[i]);
			int pos = 0, remain = size * 2 + 1;
			while (remain > 0)                                // Adler32 : mod tous les <= 5552 octets
			{
				int c2 = remain > 5552 ? 5552 : remain;
				for (int i = 0; i < c2; i++) { aA += filt[pos + i]; aB += aA; }
				aA %= 65521; aB %= 65521; pos += c2; remain -= c2;
			}
			ds.Write(filt, 0, size * 2 + 1);
			byte[] tmp = prev; prev = cur; cur = tmp;
		}
		ds.Close();
		comp.Write(BE((aB << 16) | aA), 0, 4);                // Adler32 final du zlib

		using (FileStream fs = File.Create(pngPath))
		{
			fs.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }, 0, 8);
			MemoryStream ih = new MemoryStream();
			ih.Write(BE((uint)size), 0, 4); ih.Write(BE((uint)size), 0, 4);
			ih.WriteByte(16); ih.WriteByte(0);                // 16 bits, gris
			ih.WriteByte(0); ih.WriteByte(0); ih.WriteByte(0);
			Chunk(fs, "IHDR", ih.ToArray());
			Chunk(fs, "IDAT", comp.ToArray());
			Chunk(fs, "IEND", new byte[0]);
		}
		return new double[] { min, max, sum / (n - nod), nod, capitole };
	}
}
'@

Write-Host "Assemblage $SizePx x $SizePx -> PNG 16 bits..."
$PngPath = Join-Path $PSScriptRoot 'toulouse10_mnt.png'
$Stats = [MntPng]::Assemble($OutDir, $PngPath, $Tiles, $TilePx)
$MinA = $Stats[0]; $MaxA = $Stats[1]; $MoyA = $Stats[2]; $Nodata = [long]$Stats[3]; $CapA = $Stats[4]

function F([double]$V, [int]$D = 2) { return [Math]::Round($V, $D).ToString($Inv) }
$Json = @"
{
  "source": "IGN RGE ALTI 1 m (MNT sol nu) via WMS-Raster Geoplateforme data.geopf.fr/wms-r, couche ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES, format image/x-bil;bits=32, Licence Ouverte 2.0 (attribution IGN)",
  "date_fetch": "$(Get-Date -Format 'yyyy-MM-dd')",
  "origin_wgs84": { "lat0": $($Lat0.ToString($Inv)), "lon0": $($Lon0.ToString($Inv)) },
  "bbox_wgs84": { "south": $($S.ToString($Inv)), "north": $($N.ToString($Inv)), "west": $($W.ToString($Inv)), "east": $($E.ToString($Inv)) },
  "projection_locale": {
    "type": "equirectangulaire locale, IDENTIQUE a Fetch-Toulouse10.ps1 (toulouse10*.json)",
    "m_per_deg_lat": $($MPerLat.ToString($Inv)),
    "m_per_deg_lon": $($MPerLon.ToString('0.####', $Inv)),
    "x_m": "(lon - lon0) * m_per_deg_lon  (X = est)",
    "y_m": "(lat0 - lat) * m_per_deg_lat  (NORD = -Y, Unreal main gauche, convention Cesium)"
  },
  "grid": {
    "width_px": $SizePx, "height_px": $SizePx, "pixel_size_m": 1.0,
    "coin_nw_local_m": { "x": $((-$HalfM).ToString($Inv)), "y": $((-$HalfM).ToString($Inv)) },
    "coin_nw_unreal_cm": { "x": $((-$HalfM*100).ToString($Inv)), "y": $((-$HalfM*100).ToString($Inv)) },
    "ordre_lignes": "ligne 0 = NORD (y local minimal), les lignes croissent vers le sud (+Y)",
    "centre_pixel": "x_m = -5000 + (col + 0.5) ; y_m = -5000 + (row + 0.5)"
  },
  "encoding": {
    "format": "PNG 16 bits niveaux de gris, filtre Up",
    "alt_m": "valeur / 100.0 (la valeur est l'altitude NGF en centimetres)",
    "unreal_z_cm": "valeur (1 unite PNG = 1 cm Unreal, Z vers le haut ; rebaser sur alt_capitole_m pour garder l'origine pres de z=0)",
    "nodata_source": -99999.0, "nodata_count": $Nodata, "nodata_remplacement": "altitude min valide"
  },
  "stats": { "min_alt_m": $(F $MinA), "max_alt_m": $(F $MaxA), "moy_alt_m": $(F $MoyA), "alt_capitole_m": $(F $CapA) },
  "tuiles": { "grille": "$Tiles x $Tiles", "tile_px": $TilePx, "fichiers": "MNT/mnt_1m_<tx>_<ty>.bil (float32 LE, 1 bande row-major, ligne 0 = nord, tx 0 = ouest, ty 0 = nord)" }
}
"@
$JsonPath = Join-Path $PSScriptRoot 'toulouse10_mnt.json'
[System.IO.File]::WriteAllText($JsonPath, $Json, (New-Object System.Text.UTF8Encoding $false))
Write-Host "PNG : $PngPath ($([Math]::Round((Get-Item $PngPath).Length/1MB,1)) Mo)"
Write-Host "JSON : $JsonPath"
Write-Host "Altitudes : min $(F $MinA) m / max $(F $MaxA) m / moy $(F $MoyA) m / Capitole $(F $CapA) m / nodata $Nodata"
