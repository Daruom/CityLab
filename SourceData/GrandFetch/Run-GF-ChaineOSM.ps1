# Run-GF-ChaineOSM.ps1 -- chaine B du GRAND FETCH : themes purement Overpass.
# Un processus PowerShell par theme (isolation), on continue en cas d'echec.
# Relance sure : les themes dont la sortie existe deja sont sautes.
. (Join-Path $PSScriptRoot 'GF-Common.ps1')
$ErrorActionPreference = 'Continue'

$Etapes = @(
	@{ S = 'Fetch-GF-RuesNommees.ps1'; O = 'rues_nommees.json' },
	@{ S = 'Fetch-GF-Eclairage.ps1';   O = 'eclairage.json' },
	@{ S = 'Fetch-GF-Tunnels.ps1';     O = 'tunnels.json' }
)
foreach ($E in $Etapes) {
	$Out = Join-Path $GFDir $E.O
	if (Test-Path $Out) { GFLog 'chaine-osm' "SKIP $($E.S) (sortie existante)"; continue }
	GFLog 'chaine-osm' "DEBUT $($E.S)"
	& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $GFDir $E.S)
	if ($LASTEXITCODE -ne 0) { GFLog 'chaine-osm' "ECHEC $($E.S) (exit $LASTEXITCODE) -- on continue" }
	else { GFLog 'chaine-osm' "FIN $($E.S)" }
}
GFLog 'chaine-osm' 'CHAINE OSM TERMINEE'
