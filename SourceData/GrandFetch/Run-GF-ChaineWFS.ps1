# Run-GF-ChaineWFS.ps1 -- chaine A du GRAND FETCH : themes touchant le WFS
# Geoplateforme (un seul flux WFS a la fois : le serveur jette les rafales).
# Les themes mixtes (ferre, equipements, poi) sont en fin de chaine pour que leur
# part Overpass tombe apres la chaine OSM. Un processus par theme, echec non bloquant.
# Relance sure : sortie existante = theme saute. -Skip pour exclure des scripts
# (ex. : deja lances a la main).
param([string[]]$Skip = @())
. (Join-Path $PSScriptRoot 'GF-Common.ps1')
$ErrorActionPreference = 'Continue'

$Etapes = @(
	@{ S = 'Fetch-GF-BatiEnrichi.ps1';    O = 'bati_enrichi.json' },
	@{ S = 'Fetch-GF-Aerodrome.ps1';      O = 'aerodrome.json' },
	@{ S = 'Fetch-GF-Electrique.ps1';     O = 'electrique.json' },
	@{ S = 'Fetch-GF-HydroAxes.ps1';      O = 'hydro_axes.json' },
	@{ S = 'Fetch-GF-Admin.ps1';          O = 'admin.json' },
	@{ S = 'Fetch-GF-EchantillonsJ5.ps1'; O = 'echantillons_j5\haie.json' },
	@{ S = 'Fetch-GF-Ferre.ps1';          O = 'ferre.json' },
	@{ S = 'Fetch-GF-Equipements.ps1';    O = 'equipements.json' },
	@{ S = 'Fetch-GF-Poi.ps1';            O = 'poi.json' }
)
foreach ($E in $Etapes) {
	if ($Skip -contains $E.S) { GFLog 'chaine-wfs' "SKIP $($E.S) (exclu par parametre)"; continue }
	$Out = Join-Path $GFDir $E.O
	if (Test-Path $Out) { GFLog 'chaine-wfs' "SKIP $($E.S) (sortie existante)"; continue }
	GFLog 'chaine-wfs' "DEBUT $($E.S)"
	& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $GFDir $E.S)
	if ($LASTEXITCODE -ne 0) { GFLog 'chaine-wfs' "ECHEC $($E.S) (exit $LASTEXITCODE) -- on continue" }
	else { GFLog 'chaine-wfs' "FIN $($E.S)" }
}
GFLog 'chaine-wfs' 'CHAINE WFS TERMINEE'
